// Copyright 2018 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "google/cloud/storage/internal/curl_client.h"
#include "google/cloud/internal/getenv.h"
#include "google/cloud/internal/make_unique.h"
#include "google/cloud/storage/internal/curl_request_builder.h"
#include "google/cloud/storage/internal/curl_resumable_streambuf.h"
#include "google/cloud/storage/internal/curl_resumable_upload_session.h"
#include "google/cloud/storage/internal/curl_streambuf.h"
#include "google/cloud/storage/internal/generate_message_boundary.h"
#include "google/cloud/storage/object_stream.h"

namespace google {
namespace cloud {
namespace storage {
inline namespace STORAGE_CLIENT_NS {
namespace internal {
namespace {

extern "C" void CurlShareLockCallback(CURL*, curl_lock_data,
                                      curl_lock_access,
                                      void* userptr) {
  auto* client = reinterpret_cast<CurlClient*>(userptr);
  client->LockShared();
}

extern "C" void CurlShareUnlockCallback(CURL*,
                                        curl_lock_data,
                                        void* userptr) {
  auto* client = reinterpret_cast<CurlClient*>(userptr);
  client->UnlockShared();
}

std::shared_ptr<CurlHandleFactory> CreateHandleFactory(
    ClientOptions const& options) {
  if (options.connection_pool_size() == 0U) {
    return std::make_shared<DefaultCurlHandleFactory>();
  }
  return std::make_shared<PooledCurlHandleFactory>(
      options.connection_pool_size());
}

std::unique_ptr<HashValidator> CreateHashValidator(bool disable_md5,
                                                   bool disable_crc32c) {
  if (disable_md5 and disable_crc32c) {
    return google::cloud::internal::make_unique<NullHashValidator>();
  }
  if (disable_md5) {
    return google::cloud::internal::make_unique<Crc32cHashValidator>();
  }
  if (disable_crc32c) {
    return google::cloud::internal::make_unique<MD5HashValidator>();
  }
  return google::cloud::internal::make_unique<CompositeValidator>(
      google::cloud::internal::make_unique<Crc32cHashValidator>(),
      google::cloud::internal::make_unique<MD5HashValidator>());
}

/// Create a HashValidator for a download request.
std::unique_ptr<HashValidator> CreateHashValidator(
    ReadObjectRangeRequest const& request) {
  return CreateHashValidator(request.HasOption<DisableMD5Hash>(),
                             request.HasOption<DisableCrc32cChecksum>());
}

/// Create a HashValidator for an upload request.
std::unique_ptr<HashValidator> CreateHashValidator(
    InsertObjectStreamingRequest const& request) {
  return CreateHashValidator(request.HasOption<DisableMD5Hash>(),
                             request.HasOption<DisableCrc32cChecksum>());
}

/// Create a HashValidator for an insert request.
std::unique_ptr<HashValidator> CreateHashValidator(
    InsertObjectMediaRequest const&) {
  return google::cloud::internal::make_unique<NullHashValidator>();
}

std::string XmlMapPredefinedAcl(std::string const& acl) {
  static std::map<std::string, std::string> mapping{
      {"authenticatedRead", "authenticated-read"},
      {"bucketOwnerFullControl", "bucket-owner-full-control"},
      {"bucketOwnerRead", "bucket-owner-read"},
      {"private", "private"},
      {"projectPrivate", "project-private"},
      {"publicRead", "public-read"},
  };
  auto loc = mapping.find(acl);
  if (loc == mapping.end()) {
    return acl;
  }
  return loc->second;
}

std::string UrlEscapeString(std::string const& value) {
  CurlHandle handle;
  return std::string(handle.MakeEscapedString(value).get());
}

}  // namespace

Status CurlClient::SetupBuilderCommon(CurlRequestBuilder& builder,
                                      char const* method) {
  auto auth_header_pair = AuthorizationHeader(options_.credentials());
  if (not auth_header_pair.first.ok()) {
    return auth_header_pair.first;
  }
  builder.SetMethod(method)
      .SetDebugLogging(options_.enable_http_tracing())
      .SetCurlShare(share_.get())
      .AddUserAgentPrefix(options_.user_agent_prefix())
      .AddHeader(auth_header_pair.second);
  return Status();
}

template <typename Request>
Status CurlClient::SetupBuilder(CurlRequestBuilder& builder,
                                Request const& request, char const* method) {
  auto status = SetupBuilderCommon(builder, method);
  if (not status.ok()) {
    return status;
  }
  request.AddOptionsToHttpRequest(builder);
  if (request.template HasOption<UserIp>()) {
    std::string value = request.template GetOption<UserIp>().value();
    if (value.empty()) {
      value = builder.LastClientIpAddress();
    }
    if (not value.empty()) {
      builder.AddQueryParameter(UserIp::name(), value);
    }
  }
  return Status();
}

template <typename RequestType>
std::pair<Status, std::unique_ptr<ResumableUploadSession>>
CurlClient::CreateResumableSessionGeneric(RequestType const& request) {
  if (request.template HasOption<UseResumableUploadSession>()) {
    auto session_id =
        request.template GetOption<UseResumableUploadSession>().value();
    if (not session_id.empty()) {
      return RestoreResumableSession(session_id);
    }
  }

  CurlRequestBuilder builder(
      upload_endpoint_ + "/b/" + request.bucket_name() + "/o", upload_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, std::unique_ptr<ResumableUploadSession>());
  }
  builder.AddQueryParameter("uploadType", "resumable");
  builder.AddQueryParameter("name", request.object_name());
  builder.AddHeader("Content-Type: application/json; charset=UTF-8");
  std::string request_payload;
  if (request.template HasOption<WithObjectMetadata>()) {
    request_payload = request.template GetOption<WithObjectMetadata>()
                          .value()
                          .JsonPayloadForUpdate();
  }
  builder.AddHeader("Content-Length: " +
                    std::to_string(request_payload.size()));
  auto http_response = builder.BuildRequest().MakeRequest(request_payload);
  if (http_response.status_code >= 300) {
    return std::make_pair(
        Status{http_response.status_code, std::move(http_response.payload)},
        std::unique_ptr<ResumableUploadSession>());
  }
  auto response =
      ResumableUploadResponse::FromHttpResponse(std::move(http_response));
  if (response.upload_session_url.empty()) {
    std::ostringstream os;
    os << __func__ << " - invalid server response, parsed to " << response;
    return std::make_pair(Status(600, std::move(os).str()),
                          std::unique_ptr<ResumableUploadSession>());
  }
  auto session =
      google::cloud::internal::make_unique<CurlResumableUploadSession>(
          shared_from_this(), std::move(response.upload_session_url));
  return std::make_pair(Status(), std::move(session));
}

CurlClient::CurlClient(ClientOptions options)
    : options_(std::move(options)),
      share_(curl_share_init(), &curl_share_cleanup),
      generator_(google::cloud::internal::MakeDefaultPRNG()),
      storage_factory_(CreateHandleFactory(options_)),
      upload_factory_(CreateHandleFactory(options_)),
      xml_upload_factory_(CreateHandleFactory(options_)),
      xml_download_factory_(CreateHandleFactory(options_)) {
  storage_endpoint_ = options_.endpoint() + "/storage/" + options_.version();
  upload_endpoint_ =
      options_.endpoint() + "/upload/storage/" + options_.version();

  auto endpoint =
      google::cloud::internal::GetEnv("CLOUD_STORAGE_TESTBENCH_ENDPOINT");
  if (endpoint.has_value()) {
    xml_upload_endpoint_ = options_.endpoint() + "/xmlapi";
    xml_download_endpoint_ = options_.endpoint() + "/xmlapi";
  } else {
    xml_upload_endpoint_ = "https://storage-upload.googleapis.com";
    xml_download_endpoint_ = "https://storage-download.googleapis.com";
  }

  curl_share_setopt(share_.get(), CURLSHOPT_LOCKFUNC, CurlShareLockCallback);
  curl_share_setopt(share_.get(), CURLSHOPT_UNLOCKFUNC,
                    CurlShareUnlockCallback);
  curl_share_setopt(share_.get(), CURLSHOPT_USERDATA, this);
  curl_share_setopt(share_.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_CONNECT);
  curl_share_setopt(share_.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_SSL_SESSION);
  curl_share_setopt(share_.get(), CURLSHOPT_SHARE, CURL_LOCK_DATA_DNS);

  CurlInitializeOnce(options.enable_ssl_locking_callbacks());
}

std::pair<Status, ResumableUploadResponse> CurlClient::UploadChunk(
    UploadChunkRequest const& request) {
  CurlRequestBuilder builder(request.upload_session_url(), upload_factory_);
  auto status = SetupBuilder(builder, request, "PUT");
  if (not status.ok()) {
    return std::make_pair(status, ResumableUploadResponse{});
  }
  builder.AddHeader(request.RangeHeader());
  builder.AddHeader("Content-Type: application/octet-stream");
  builder.AddHeader("Content-Length: " +
                    std::to_string(request.payload().size()));
  auto payload = builder.BuildRequest().MakeRequest(request.payload());
  bool success_with_308 =
      payload.status_code == 308 and
      payload.headers.find("range") != payload.headers.end();
  if (status.ok() or success_with_308) {
    return std::make_pair(Status(), ResumableUploadResponse::FromHttpResponse(
                                        std::move(payload)));
  }
  return std::make_pair(Status{payload.status_code, std::move(payload.payload)},
                        ResumableUploadResponse{});
}

std::pair<Status, ResumableUploadResponse> CurlClient::QueryResumableUpload(
    QueryResumableUploadRequest const& request) {
  CurlRequestBuilder builder(request.upload_session_url(), upload_factory_);
  auto status = SetupBuilder(builder, request, "PUT");
  if (not status.ok()) {
    return std::make_pair(status, ResumableUploadResponse{});
  }
  builder.AddHeader("Content-Range: bytes */*");
  builder.AddHeader("Content-Type: application/octet-stream");
  builder.AddHeader("Content-Length: 0");
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  bool success_with_308 =
      payload.status_code == 308 and
      payload.headers.find("range") != payload.headers.end();
  if (status.ok() or success_with_308) {
    return std::make_pair(Status(), ResumableUploadResponse::FromHttpResponse(
                                        std::move(payload)));
  }
  return std::make_pair(Status{payload.status_code, std::move(payload.payload)},
                        ResumableUploadResponse{});
}

std::pair<Status, ListBucketsResponse> CurlClient::ListBuckets(
    ListBucketsRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b", storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, ListBucketsResponse{});
  }
  builder.AddQueryParameter("project", request.project_id());
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ListBucketsResponse{});
  }
  return std::make_pair(
      Status(), ListBucketsResponse::FromHttpResponse(std::move(payload)));
}

std::pair<Status, BucketMetadata> CurlClient::CreateBucket(
    CreateBucketRequest const& request) {
  // Assume the bucket name is validated by the caller.
  CurlRequestBuilder builder(storage_endpoint_ + "/b", storage_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, BucketMetadata{});
  }
  builder.AddQueryParameter("project", request.project_id());
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(request.json_payload());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        BucketMetadata{});
  }
  return std::make_pair(Status(),
                        BucketMetadata::ParseFromString(payload.payload));
}

std::pair<Status, BucketMetadata> CurlClient::GetBucketMetadata(
    GetBucketMetadataRequest const& request) {
  // Assume the bucket name is validated by the caller.
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name(),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, BucketMetadata{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (200 != payload.status_code) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        BucketMetadata{});
  }
  return std::make_pair(Status(),
                        BucketMetadata::ParseFromString(payload.payload));
}

std::pair<Status, EmptyResponse> CurlClient::DeleteBucket(
    DeleteBucketRequest const& request) {
  // Assume the bucket name is validated by the caller.
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name(),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "DELETE");
  if (not status.ok()) {
    return std::make_pair(status, EmptyResponse{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        internal::EmptyResponse{});
  }
  return std::make_pair(Status(), internal::EmptyResponse{});
}

std::pair<Status, BucketMetadata> CurlClient::UpdateBucket(
    UpdateBucketRequest const& request) {
  // Assume the bucket name is validated by the caller.
  CurlRequestBuilder builder(
      storage_endpoint_ + "/b/" + request.metadata().name(), storage_factory_);
  auto status = SetupBuilder(builder, request, "PUT");
  if (not status.ok()) {
    return std::make_pair(status, BucketMetadata{});
  }
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(request.json_payload());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        BucketMetadata{});
  }
  return std::make_pair(Status(),
                        BucketMetadata::ParseFromString(payload.payload));
}

std::pair<Status, BucketMetadata> CurlClient::PatchBucket(
    PatchBucketRequest const& request) {
  // Assume the bucket name is validated by the caller.
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket(),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "PATCH");
  if (not status.ok()) {
    return std::make_pair(status, BucketMetadata{});
  }
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(request.payload());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        BucketMetadata{});
  }
  return std::make_pair(Status(),
                        BucketMetadata::ParseFromString(payload.payload));
}

std::pair<Status, IamPolicy> CurlClient::GetBucketIamPolicy(
    GetBucketIamPolicyRequest const& request) {
  CurlRequestBuilder builder(
      storage_endpoint_ + "/b/" + request.bucket_name() + "/iam",
      storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, IamPolicy{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)}, IamPolicy{});
  }
  return std::make_pair(Status(), ParseIamPolicyFromString(payload.payload));
}

std::pair<Status, IamPolicy> CurlClient::SetBucketIamPolicy(
    SetBucketIamPolicyRequest const& request) {
  CurlRequestBuilder builder(
      storage_endpoint_ + "/b/" + request.bucket_name() + "/iam",
      storage_factory_);
  auto status = SetupBuilder(builder, request, "PUT");
  if (not status.ok()) {
    return std::make_pair(status, IamPolicy{});
  }
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(request.json_payload());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)}, IamPolicy{});
  }
  return std::make_pair(Status(), ParseIamPolicyFromString(payload.payload));
}

std::pair<Status, TestBucketIamPermissionsResponse>
CurlClient::TestBucketIamPermissions(
    google::cloud::storage::internal::TestBucketIamPermissionsRequest const&
        request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/iam/testPermissions",
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, TestBucketIamPermissionsResponse{});
  }
  for (auto const& perm : request.permissions()) {
    builder.AddQueryParameter("permissions", perm);
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        TestBucketIamPermissionsResponse{});
  }
  return std::make_pair(
      Status(), TestBucketIamPermissionsResponse::FromHttpResponse(payload));
}

std::pair<Status, EmptyResponse> CurlClient::LockBucketRetentionPolicy(
    LockBucketRetentionPolicyRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/lockRetentionPolicy",
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, EmptyResponse{});
  }
  builder.AddHeader("content-type: application/json");
  builder.AddHeader("content-length: 0");
  builder.AddOption(IfMetagenerationMatch(request.metageneration()));
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        EmptyResponse{});
  }
  return std::make_pair(Status(), EmptyResponse{});
}

std::pair<Status, ObjectMetadata> CurlClient::InsertObjectMedia(
    InsertObjectMediaRequest const& request) {
  // If the object metadata is specified, then we need to do a multipart upload.
  if (request.HasOption<WithObjectMetadata>()) {
    return InsertObjectMediaMultipart(request);
  }

  // Unless the request uses a feature that disables it, prefer to use XML.
  if (not request.HasOption<IfMetagenerationNotMatch>() and
      not request.HasOption<IfGenerationNotMatch>() and
      not request.HasOption<QuotaUser>() and not request.HasOption<UserIp>() and
      not request.HasOption<Projection>() and request.HasOption<Fields>() and
      request.GetOption<Fields>().value().empty()) {
    return InsertObjectMediaXml(request);
  }

  // If the application has set an explicit hash value we need to use multipart
  // uploads.
  if (not request.HasOption<DisableMD5Hash>() and
      not request.HasOption<DisableCrc32cChecksum>()) {
    return InsertObjectMediaMultipart(request);
  }

  // Otherwise do a simple upload.
  return InsertObjectMediaSimple(request);
}

std::pair<Status, ObjectMetadata> CurlClient::CopyObject(
    CopyObjectRequest const& request) {
  CurlRequestBuilder builder(
      storage_endpoint_ + "/b/" + request.source_bucket() + "/o/" +
          UrlEscapeString(request.source_object()) + "/copyTo/b/" +
          request.destination_bucket() + "/o/" +
          UrlEscapeString(request.destination_object()),
      storage_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, ObjectMetadata{});
  }
  builder.AddHeader("Content-Type: application/json");
  std::string json_payload("{}");
  if (request.HasOption<WithObjectMetadata>()) {
    json_payload =
        request.GetOption<WithObjectMetadata>().value().JsonPayloadForCopy();
  }
  auto payload = builder.BuildRequest().MakeRequest(json_payload);
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectMetadata{});
  }
  return std::make_pair(Status(),
                        ObjectMetadata::ParseFromString(payload.payload));
}

std::pair<Status, ObjectMetadata> CurlClient::GetObjectMetadata(
    GetObjectMetadataRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, ObjectMetadata{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectMetadata{});
  }
  return std::make_pair(Status(),
                        ObjectMetadata::ParseFromString(payload.payload));
}

std::pair<Status, std::unique_ptr<ObjectReadStreambuf>> CurlClient::ReadObject(
    ReadObjectRangeRequest const& request) {
  if (not request.HasOption<IfMetagenerationNotMatch>() and
      not request.HasOption<IfGenerationNotMatch>() and
      not request.HasOption<QuotaUser>() and not request.HasOption<UserIp>()) {
    return ReadObjectXml(request);
  }
  // Assume the bucket name is validated by the caller.
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status,
                          std::unique_ptr<ObjectReadStreambuf>(nullptr));
  }
  builder.AddQueryParameter("alt", "media");

  std::unique_ptr<CurlReadStreambuf> buf(new CurlReadStreambuf(
      builder.BuildDownloadRequest(std::string{}),
      client_options().download_buffer_size(), CreateHashValidator(request)));
  return std::make_pair(Status(),
                        std::unique_ptr<ObjectReadStreambuf>(std::move(buf)));
}

std::pair<Status, std::unique_ptr<ObjectWriteStreambuf>>
CurlClient::WriteObject(InsertObjectStreamingRequest const& request) {
  if (not request.HasOption<IfMetagenerationNotMatch>() and
      not request.HasOption<IfGenerationNotMatch>() and
      not request.HasOption<QuotaUser>() and not request.HasOption<UserIp>() and
      not request.HasOption<Projection>() and request.HasOption<Fields>() and
      request.GetOption<Fields>().value().empty()) {
    return WriteObjectXml(request);
  }

  if (request.HasOption<WithObjectMetadata>() or
      request.HasOption<UseResumableUploadSession>()) {
    return WriteObjectResumable(request);
  }

  return WriteObjectSimple(request);
}

std::pair<Status, ListObjectsResponse> CurlClient::ListObjects(
    ListObjectsRequest const& request) {
  // Assume the bucket name is validated by the caller.
  CurlRequestBuilder builder(
      storage_endpoint_ + "/b/" + request.bucket_name() + "/o",
      storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, ListObjectsResponse{});
  }
  builder.AddQueryParameter("pageToken", request.page_token());
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (200 != payload.status_code) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        internal::ListObjectsResponse{});
  }
  return std::make_pair(
      Status(),
      internal::ListObjectsResponse::FromHttpResponse(std::move(payload)));
}

std::pair<Status, EmptyResponse> CurlClient::DeleteObject(
    DeleteObjectRequest const& request) {
  // Assume the bucket name is validated by the caller.
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "DELETE");
  if (not status.ok()) {
    return std::make_pair(status, EmptyResponse{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        internal::EmptyResponse{});
  }
  return std::make_pair(Status(), internal::EmptyResponse{});
}

std::pair<Status, ObjectMetadata> CurlClient::UpdateObject(
    UpdateObjectRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "PUT");
  if (not status.ok()) {
    return std::make_pair(status, ObjectMetadata{});
  }
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(request.json_payload());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectMetadata{});
  }
  return std::make_pair(Status(),
                        ObjectMetadata::ParseFromString(payload.payload));
}

std::pair<Status, ObjectMetadata> CurlClient::PatchObject(
    PatchObjectRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "PATCH");
  if (not status.ok()) {
    return std::make_pair(status, ObjectMetadata{});
  }
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(request.payload());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectMetadata{});
  }
  return std::make_pair(Status(),
                        ObjectMetadata::ParseFromString(payload.payload));
}

std::pair<Status, ObjectMetadata> CurlClient::ComposeObject(
    ComposeObjectRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()) + "/compose",
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, ObjectMetadata{});
  }
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(request.JsonPayload());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectMetadata{});
  }
  return std::make_pair(Status(),
                        ObjectMetadata::ParseFromString(payload.payload));
}

std::pair<Status, RewriteObjectResponse> CurlClient::RewriteObject(
    RewriteObjectRequest const& request) {
  CurlRequestBuilder builder(
      storage_endpoint_ + "/b/" + request.source_bucket() + "/o/" +
          UrlEscapeString(request.source_object()) + "/rewriteTo/b/" +
          request.destination_bucket() + "/o/" +
          UrlEscapeString(request.destination_object()),
      storage_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, RewriteObjectResponse{});
  }
  if (not request.rewrite_token().empty()) {
    builder.AddQueryParameter("rewriteToken", request.rewrite_token());
  }
  builder.AddHeader("Content-Type: application/json");
  std::string json_payload("{}");
  if (request.HasOption<WithObjectMetadata>()) {
    json_payload =
        request.GetOption<WithObjectMetadata>().value().JsonPayloadForCopy();
  }
  auto payload = builder.BuildRequest().MakeRequest(json_payload);
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        RewriteObjectResponse{});
  }
  return std::make_pair(Status(),
                        RewriteObjectResponse::FromHttpResponse(payload));
}

std::pair<Status, std::unique_ptr<ResumableUploadSession>>
CurlClient::CreateResumableSession(ResumableUploadRequest const& request) {
  return CreateResumableSessionGeneric(request);
}

std::pair<Status, std::unique_ptr<ResumableUploadSession>>
CurlClient::RestoreResumableSession(std::string const& session_id) {
  auto session =
      google::cloud::internal::make_unique<CurlResumableUploadSession>(
          shared_from_this(), session_id);
  auto response = session->ResetSession();
  if (response.first.status_code() == 308 or
      response.first.status_code() < 300) {
    return std::make_pair(Status(), std::move(session));
  }
  return std::make_pair(std::move(response.first),
                        std::unique_ptr<ResumableUploadSession>());
}

std::pair<Status, ListBucketAclResponse> CurlClient::ListBucketAcl(
    ListBucketAclRequest const& request) {
  CurlRequestBuilder builder(
      storage_endpoint_ + "/b/" + request.bucket_name() + "/acl",
      storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, ListBucketAclResponse{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        internal::ListBucketAclResponse{});
  }
  return std::make_pair(
      Status(),
      internal::ListBucketAclResponse::FromHttpResponse(std::move(payload)));
}

std::pair<Status, BucketAccessControl> CurlClient::GetBucketAcl(
    GetBucketAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/acl/" + UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, BucketAccessControl{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        BucketAccessControl{});
  }
  return std::make_pair(Status(),
                        BucketAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, BucketAccessControl> CurlClient::CreateBucketAcl(
    CreateBucketAclRequest const& request) {
  CurlRequestBuilder builder(
      storage_endpoint_ + "/b/" + request.bucket_name() + "/acl",
      storage_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, BucketAccessControl{});
  }
  builder.AddHeader("Content-Type: application/json");
  nl::json object;
  object["entity"] = request.entity();
  object["role"] = request.role();
  auto payload = builder.BuildRequest().MakeRequest(object.dump());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        BucketAccessControl{});
  }
  return std::make_pair(Status(),
                        BucketAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, EmptyResponse> CurlClient::DeleteBucketAcl(
    DeleteBucketAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/acl/" + UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "DELETE");
  if (not status.ok()) {
    return std::make_pair(status, EmptyResponse{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        internal::EmptyResponse{});
  }
  return std::make_pair(Status(), internal::EmptyResponse{});
}

std::pair<Status, BucketAccessControl> CurlClient::UpdateBucketAcl(
    UpdateBucketAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/acl/" + UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "PUT");
  if (not status.ok()) {
    return std::make_pair(status, BucketAccessControl{});
  }
  builder.AddHeader("Content-Type: application/json");
  nl::json patch;
  patch["entity"] = request.entity();
  patch["role"] = request.role();
  auto payload = builder.BuildRequest().MakeRequest(patch.dump());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        BucketAccessControl{});
  }
  return std::make_pair(Status(),
                        BucketAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, BucketAccessControl> CurlClient::PatchBucketAcl(
    PatchBucketAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/acl/" + UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "PATCH");
  if (not status.ok()) {
    return std::make_pair(status, BucketAccessControl{});
  }
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(request.payload());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        BucketAccessControl{});
  }
  return std::make_pair(Status(),
                        BucketAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, ListObjectAclResponse> CurlClient::ListObjectAcl(
    ListObjectAclRequest const& request) {
  // Assume the bucket name is validated by the caller.
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()) + "/acl",
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, ListObjectAclResponse{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        internal::ListObjectAclResponse{});
  }
  return std::make_pair(
      Status(),
      internal::ListObjectAclResponse::FromHttpResponse(std::move(payload)));
}

std::pair<Status, ObjectAccessControl> CurlClient::CreateObjectAcl(
    CreateObjectAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()) + "/acl",
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, ObjectAccessControl{});
  }
  builder.AddHeader("Content-Type: application/json");
  nl::json object;
  object["entity"] = request.entity();
  object["role"] = request.role();
  auto payload = builder.BuildRequest().MakeRequest(object.dump());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectAccessControl{});
  }
  return std::make_pair(Status(),
                        ObjectAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, EmptyResponse> CurlClient::DeleteObjectAcl(
    DeleteObjectAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()) + "/acl/" +
                                 UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "DELETE");
  if (not status.ok()) {
    return std::make_pair(status, EmptyResponse{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        internal::EmptyResponse{});
  }
  return std::make_pair(Status(), internal::EmptyResponse{});
}

std::pair<Status, ObjectAccessControl> CurlClient::GetObjectAcl(
    GetObjectAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()) + "/acl/" +
                                 UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, ObjectAccessControl{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectAccessControl{});
  }
  return std::make_pair(Status(),
                        ObjectAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, ObjectAccessControl> CurlClient::UpdateObjectAcl(
    UpdateObjectAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()) + "/acl/" +
                                 UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "PUT");
  if (not status.ok()) {
    return std::make_pair(status, ObjectAccessControl{});
  }
  builder.AddHeader("Content-Type: application/json");
  nl::json object;
  object["entity"] = request.entity();
  object["role"] = request.role();
  auto payload = builder.BuildRequest().MakeRequest(object.dump());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectAccessControl{});
  }
  return std::make_pair(Status(),
                        ObjectAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, ObjectAccessControl> CurlClient::PatchObjectAcl(
    PatchObjectAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/o/" + UrlEscapeString(request.object_name()) + "/acl/" +
                                 UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "PATCH");
  if (not status.ok()) {
    return std::make_pair(status, ObjectAccessControl{});
  }
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(request.payload());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectAccessControl{});
  }
  return std::make_pair(Status(),
                        ObjectAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, ListDefaultObjectAclResponse>
CurlClient::ListDefaultObjectAcl(ListDefaultObjectAclRequest const& request) {
  // Assume the bucket name is validated by the caller.
  CurlRequestBuilder builder(
      storage_endpoint_ + "/b/" + request.bucket_name() + "/defaultObjectAcl",
      storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, ListDefaultObjectAclResponse{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        internal::ListDefaultObjectAclResponse{});
  }
  return std::make_pair(
      Status(), internal::ListDefaultObjectAclResponse::FromHttpResponse(
                    std::move(payload)));
}

std::pair<Status, ObjectAccessControl> CurlClient::CreateDefaultObjectAcl(
    CreateDefaultObjectAclRequest const& request) {
  CurlRequestBuilder builder(
      storage_endpoint_ + "/b/" + request.bucket_name() + "/defaultObjectAcl",
      storage_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, ObjectAccessControl{});
  }
  nl::json object;
  object["entity"] = request.entity();
  object["role"] = request.role();
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(object.dump());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectAccessControl{});
  }
  return std::make_pair(Status(),
                        ObjectAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, EmptyResponse> CurlClient::DeleteDefaultObjectAcl(
    DeleteDefaultObjectAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/defaultObjectAcl/" +
                                 UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "DELETE");
  if (not status.ok()) {
    return std::make_pair(status, EmptyResponse{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        internal::EmptyResponse{});
  }
  return std::make_pair(Status(), internal::EmptyResponse{});
}

std::pair<Status, ObjectAccessControl> CurlClient::GetDefaultObjectAcl(
    GetDefaultObjectAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/defaultObjectAcl/" +
                                 UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, ObjectAccessControl{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectAccessControl{});
  }
  return std::make_pair(Status(),
                        ObjectAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, ObjectAccessControl> CurlClient::UpdateDefaultObjectAcl(
    UpdateDefaultObjectAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/defaultObjectAcl/" +
                                 UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "PUT");
  if (not status.ok()) {
    return std::make_pair(status, ObjectAccessControl{});
  }
  builder.AddHeader("Content-Type: application/json");
  nl::json object;
  object["entity"] = request.entity();
  object["role"] = request.role();
  auto payload = builder.BuildRequest().MakeRequest(object.dump());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectAccessControl{});
  }
  return std::make_pair(Status(),
                        ObjectAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, ObjectAccessControl> CurlClient::PatchDefaultObjectAcl(
    PatchDefaultObjectAclRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/defaultObjectAcl/" +
                                 UrlEscapeString(request.entity()),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "PATCH");
  if (not status.ok()) {
    return std::make_pair(status, ObjectAccessControl{});
  }
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(request.payload());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectAccessControl{});
  }
  return std::make_pair(Status(),
                        ObjectAccessControl::ParseFromString(payload.payload));
}

std::pair<Status, ServiceAccount> CurlClient::GetServiceAccount(
    GetProjectServiceAccountRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/projects/" +
                                 request.project_id() + "/serviceAccount",
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, ServiceAccount{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ServiceAccount{});
  }
  return std::make_pair(Status(),
                        ServiceAccount::ParseFromString(payload.payload));
}

std::pair<Status, ListNotificationsResponse> CurlClient::ListNotifications(
    ListNotificationsRequest const& request) {
  // Assume the bucket name is validated by the caller.
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/notificationConfigs",
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, ListNotificationsResponse{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        internal::ListNotificationsResponse{});
  }
  return std::make_pair(Status(),
                        internal::ListNotificationsResponse::FromHttpResponse(
                            std::move(payload)));
}

std::pair<Status, NotificationMetadata> CurlClient::CreateNotification(
    CreateNotificationRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/notificationConfigs",
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, NotificationMetadata{});
  }
  builder.AddHeader("Content-Type: application/json");
  auto payload = builder.BuildRequest().MakeRequest(request.json_payload());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        NotificationMetadata{});
  }
  return std::make_pair(Status(),
                        NotificationMetadata::ParseFromString(payload.payload));
}

std::pair<Status, NotificationMetadata> CurlClient::GetNotification(
    GetNotificationRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/notificationConfigs/" +
                                 request.notification_id(),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "GET");
  if (not status.ok()) {
    return std::make_pair(status, NotificationMetadata{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        NotificationMetadata{});
  }
  return std::make_pair(Status(),
                        NotificationMetadata::ParseFromString(payload.payload));
}

std::pair<Status, EmptyResponse> CurlClient::DeleteNotification(
    DeleteNotificationRequest const& request) {
  CurlRequestBuilder builder(storage_endpoint_ + "/b/" + request.bucket_name() +
                                 "/notificationConfigs/" +
                                 request.notification_id(),
                             storage_factory_);
  auto status = SetupBuilder(builder, request, "DELETE");
  if (not status.ok()) {
    return std::make_pair(status, EmptyResponse{});
  }
  auto payload = builder.BuildRequest().MakeRequest(std::string{});
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        EmptyResponse{});
  }
  return std::make_pair(Status(), EmptyResponse{});
}

void CurlClient::LockShared() { mu_.lock(); }

void CurlClient::UnlockShared() { mu_.unlock(); }

std::pair<Status, ObjectMetadata> CurlClient::InsertObjectMediaXml(
    InsertObjectMediaRequest const& request) {
  CurlRequestBuilder builder(xml_upload_endpoint_ + "/" +
                                 request.bucket_name() + "/" +
                                 UrlEscapeString(request.object_name()),
                             xml_upload_factory_);
  auto status = SetupBuilderCommon(builder, "PUT");
  if (not status.ok()) {
    return std::make_pair(status, ObjectMetadata{});
  }
  builder.AddHeader("Host: storage.googleapis.com");

  //
  // Apply the options from InsertObjectMediaRequest that are set, translating
  // to the XML format for them.
  //
  builder.AddOption(request.GetOption<ContentEncoding>());
  // Set the content type of a sensible value, the application can override this
  // in the options for the request.
  if (not request.HasOption<ContentType>()) {
    builder.AddHeader("content-type: application/octet-stream");
  } else {
    builder.AddOption(request.GetOption<ContentType>());
  }
  builder.AddOption(request.GetOption<EncryptionKey>());
  if (request.HasOption<IfGenerationMatch>()) {
    builder.AddHeader(
        "x-goog-if-generation-match: " +
        std::to_string(request.GetOption<IfGenerationMatch>().value()));
  }
  // IfGenerationNotMatch cannot be set, checked by the caller.
  if (request.HasOption<IfMetagenerationMatch>()) {
    builder.AddHeader(
        "x-goog-if-meta-generation-match: " +
        std::to_string(request.GetOption<IfMetagenerationMatch>().value()));
  }
  // IfMetagenerationNotMatch cannot be set, checked by the caller.
  if (request.HasOption<KmsKeyName>()) {
    builder.AddHeader("x-goog-encryption-kms-key-name: " +
                      request.GetOption<KmsKeyName>().value());
  }
  if (request.HasOption<MD5HashValue>()) {
    builder.AddHeader("x-goog-hash: md5=" +
                      request.GetOption<MD5HashValue>().value());
  } else if (not request.HasOption<DisableMD5Hash>()) {
    builder.AddHeader("x-goog-hash: md5=" + ComputeMD5Hash(request.contents()));
  }
  if (request.HasOption<Crc32cChecksumValue>()) {
    builder.AddHeader("x-goog-hash: crc32c=" +
                      request.GetOption<Crc32cChecksumValue>().value());
  } else if (not request.HasOption<DisableCrc32cChecksum>()) {
    builder.AddHeader("x-goog-hash: crc32c=" +
                      ComputeCrc32cChecksum(request.contents()));
  }
  if (request.HasOption<PredefinedAcl>()) {
    builder.AddHeader(
        "x-goog-acl: " +
        XmlMapPredefinedAcl(request.GetOption<PredefinedAcl>().value()));
  }
  builder.AddOption(request.GetOption<UserProject>());

  //
  // Apply the options from GenericRequestBase<> that are set, translating
  // to the XML format for them.
  //
  // Fields cannot be set, checked by the caller.
  builder.AddOption(request.GetOption<CustomHeader>());
  builder.AddOption(request.GetOption<IfMatchEtag>());
  builder.AddOption(request.GetOption<IfNoneMatchEtag>());
  // QuotaUser cannot be set, checked by the caller.
  // UserIp cannot be set, checked by the caller.

  builder.AddHeader("Content-Length: " +
                    std::to_string(request.contents().size()));
  auto payload = builder.BuildRequest().MakeRequest(request.contents());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectMetadata{});
  }
  return std::make_pair(Status(),
                        ObjectMetadata::ParseFromJson(internal::nl::json{
                            {"name", request.object_name()},
                            {"bucket", request.bucket_name()},
                        }));
}

std::pair<Status, std::unique_ptr<ObjectReadStreambuf>>
CurlClient::ReadObjectXml(ReadObjectRangeRequest const& request) {
  CurlRequestBuilder builder(xml_download_endpoint_ + "/" +
                                 request.bucket_name() + "/" +
                                 UrlEscapeString(request.object_name()),
                             xml_download_factory_);
  auto status = SetupBuilderCommon(builder, "GET");
  if (not status.ok()) {
    return std::make_pair(status,
                          std::unique_ptr<ObjectReadStreambuf>(nullptr));
  }
  builder.AddHeader("Host: storage.googleapis.com");

  //
  // Apply the options from ReadObjectMediaRequest that are set, translating
  // to the XML format for them.
  //
  builder.AddOption(request.GetOption<EncryptionKey>());
  builder.AddOption(request.GetOption<Generation>());
  if (request.HasOption<IfGenerationMatch>()) {
    builder.AddHeader(
        "x-goog-if-generation-match: " +
        std::to_string(request.GetOption<IfGenerationMatch>().value()));
  }
  // IfGenerationNotMatch cannot be set, checked by the caller.
  if (request.HasOption<IfMetagenerationMatch>()) {
    builder.AddHeader(
        "x-goog-if-meta-generation-match: " +
        std::to_string(request.GetOption<IfMetagenerationMatch>().value()));
  }
  // IfMetagenerationNotMatch cannot be set, checked by the caller.
  builder.AddOption(request.GetOption<UserProject>());

  //
  // Apply the options from GenericRequestBase<> that are set, translating
  // to the XML format for them.
  //
  builder.AddOption(request.GetOption<CustomHeader>());
  builder.AddOption(request.GetOption<IfMatchEtag>());
  builder.AddOption(request.GetOption<IfNoneMatchEtag>());
  // QuotaUser cannot be set, checked by the caller.
  // UserIp cannot be set, checked by the caller.

  std::unique_ptr<CurlReadStreambuf> buf(new CurlReadStreambuf(
      builder.BuildDownloadRequest(std::string{}),
      client_options().download_buffer_size(), CreateHashValidator(request)));
  return std::make_pair(Status(),
                        std::unique_ptr<ObjectReadStreambuf>(std::move(buf)));
}

std::pair<Status, std::unique_ptr<ObjectWriteStreambuf>>
CurlClient::WriteObjectXml(InsertObjectStreamingRequest const& request) {
  CurlRequestBuilder builder(xml_upload_endpoint_ + "/" +
                                 request.bucket_name() + "/" +
                                 UrlEscapeString(request.object_name()),
                             xml_upload_factory_);
  auto status = SetupBuilderCommon(builder, "PUT");
  if (not status.ok()) {
    return std::make_pair(status,
                          std::unique_ptr<ObjectWriteStreambuf>(nullptr));
  }
  builder.AddHeader("Host: storage.googleapis.com");

  //
  // Apply the options from InsertObjectMediaRequest that are set, translating
  // to the XML format for them.
  //
  builder.AddOption(request.GetOption<ContentEncoding>());
  // Set the content type of a sensible value, the application can override this
  // in the options for the request.
  if (not request.HasOption<ContentType>()) {
    builder.AddHeader("content-type: application/octet-stream");
  } else {
    builder.AddOption(request.GetOption<ContentType>());
  }
  builder.AddOption(request.GetOption<EncryptionKey>());
  if (request.HasOption<IfGenerationMatch>()) {
    builder.AddHeader(
        "x-goog-if-generation-match: " +
        std::to_string(request.GetOption<IfGenerationMatch>().value()));
  }
  // IfGenerationNotMatch cannot be set, checked by the caller.
  if (request.HasOption<IfMetagenerationMatch>()) {
    builder.AddHeader(
        "x-goog-if-meta-generation-match: " +
        std::to_string(request.GetOption<IfMetagenerationMatch>().value()));
  }
  // IfMetagenerationNotMatch cannot be set, checked by the caller.
  if (request.HasOption<KmsKeyName>()) {
    builder.AddHeader("x-goog-encryption-kms-key-name: " +
                      request.GetOption<KmsKeyName>().value());
  }
  if (request.HasOption<PredefinedAcl>()) {
    builder.AddHeader(
        "x-goog-acl: " +
        XmlMapPredefinedAcl(request.GetOption<PredefinedAcl>().value()));
  }
  builder.AddOption(request.GetOption<UserProject>());

  //
  // Apply the options from GenericRequestBase<> that are set, translating
  // to the XML format for them.
  //
  // Fields cannot be set, checked by the caller.
  builder.AddOption(request.GetOption<CustomHeader>());
  builder.AddOption(request.GetOption<IfMatchEtag>());
  builder.AddOption(request.GetOption<IfNoneMatchEtag>());
  // QuotaUser cannot be set, checked by the caller.
  // UserIp cannot be set, checked by the caller.

  std::unique_ptr<internal::CurlWriteStreambuf> buf(
      new internal::CurlWriteStreambuf(builder.BuildUpload(),
                                       client_options().upload_buffer_size(),
                                       CreateHashValidator(request)));
  return std::make_pair(
      Status(),
      std::unique_ptr<internal::ObjectWriteStreambuf>(std::move(buf)));
}

std::pair<Status, ObjectMetadata> CurlClient::InsertObjectMediaMultipart(
    InsertObjectMediaRequest const& request) {
  // To perform a multipart upload we need to separate the parts using:
  //   https://cloud.google.com/storage/docs/json_api/v1/how-tos/multipart-upload
  // This function is structured as follows:
  // 1. Create a request object, as we often do.
  CurlRequestBuilder builder(
      upload_endpoint_ + "/b/" + request.bucket_name() + "/o", upload_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, ObjectMetadata{});
  }

  // 2. Pick a separator that does not conflict with the request contents.
  auto boundary = PickBoundary(request.contents());
  builder.AddHeader("content-type: multipart/related; boundary=" + boundary);
  builder.AddQueryParameter("uploadType", "multipart");
  builder.AddQueryParameter("name", request.object_name());

  // 3. Perform a streaming upload because computing the size upfront is more
  //    complicated than it is worth.
  std::unique_ptr<internal::CurlWriteStreambuf> buf(
      new internal::CurlWriteStreambuf(builder.BuildUpload(),
                                       client_options().upload_buffer_size(),
                                       CreateHashValidator(request)));
  ObjectWriteStream writer(std::move(buf));

  nl::json metadata = nl::json::object();
  if (request.HasOption<WithObjectMetadata>()) {
    metadata = request.GetOption<WithObjectMetadata>().value().JsonForUpdate();
  }
  if (request.HasOption<MD5HashValue>()) {
    metadata["md5Hash"] = request.GetOption<MD5HashValue>().value();
  } else {
    metadata["md5Hash"] = ComputeMD5Hash(request.contents());
  }

  if (request.HasOption<Crc32cChecksumValue>()) {
    metadata["crc32c"] = request.GetOption<Crc32cChecksumValue>().value();
  } else {
    metadata["crc32c"] = ComputeCrc32cChecksum(request.contents());
  }

  std::string crlf = "\r\n";
  std::string marker = "--" + boundary;

  // 4. Format the first part, including the separators and the headers.
  writer << marker << crlf << "content-type: application/json; charset=UTF-8"
         << crlf << crlf << metadata.dump() << crlf << marker << crlf;

  // 5. Format the second part, which includes all the contents and a final
  //    separator.
  if (request.HasOption<ContentType>()) {
    writer << "content-type: " << request.GetOption<ContentType>().value()
           << crlf;
  } else if (metadata.count("contentType") != 0) {
    writer << "content-type: "
           << metadata.value("contentType", "application/octet-stream") << crlf;
  } else {
    writer << "content-type: application/octet-stream" << crlf;
  }
  writer << crlf << request.contents() << crlf << marker << "--" << crlf;

  // 6. Return the results as usual.
  auto payload = writer.CloseRaw();
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectMetadata{});
  }
  return std::make_pair(Status(),
                        ObjectMetadata::ParseFromString(payload.payload));
}

std::string CurlClient::PickBoundary(std::string const& text_to_avoid) {
  // We need to find a string that is *not* found in `text_to_avoid`, we pick
  // a string at random, and see if it is in `text_to_avoid`, if it is, we grow
  // the string with random characters and start from where we last found a
  // the candidate.  Eventually we will find something, though it might be
  // larger than `text_to_avoid`.  And we only make (approximately) one pass
  // over `text_to_avoid`.
  auto generate_candidate = [this](int n) {
    static std::string const chars =
        "abcdefghijklmnopqrstuvwxyz012456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::unique_lock<std::mutex> lk(mu_);
    return google::cloud::internal::Sample(generator_, n, chars);
  };
  constexpr int INITIAL_CANDIDATE_SIZE = 16;
  constexpr int CANDIDATE_GROWTH_SIZE = 4;
  return GenerateMessageBoundary(text_to_avoid, std::move(generate_candidate),
                                 INITIAL_CANDIDATE_SIZE, CANDIDATE_GROWTH_SIZE);
}

std::pair<Status, ObjectMetadata> CurlClient::InsertObjectMediaSimple(
    InsertObjectMediaRequest const& request) {
  CurlRequestBuilder builder(
      upload_endpoint_ + "/b/" + request.bucket_name() + "/o", upload_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status, ObjectMetadata{});
  }
  // Set the content type of a sensible value, the application can override this
  // in the options for the request.
  if (not request.HasOption<ContentType>()) {
    builder.AddHeader("content-type: application/octet-stream");
  }
  builder.AddQueryParameter("uploadType", "media");
  builder.AddQueryParameter("name", request.object_name());
  builder.AddHeader("Content-Length: " +
                    std::to_string(request.contents().size()));
  auto payload = builder.BuildRequest().MakeRequest(request.contents());
  if (payload.status_code >= 300) {
    return std::make_pair(
        Status{payload.status_code, std::move(payload.payload)},
        ObjectMetadata{});
  }
  return std::make_pair(Status(),
                        ObjectMetadata::ParseFromString(payload.payload));
}

std::pair<Status, std::unique_ptr<ObjectWriteStreambuf>>
CurlClient::WriteObjectSimple(InsertObjectStreamingRequest const& request) {
  auto url = upload_endpoint_ + "/b/" + request.bucket_name() + "/o";
  CurlRequestBuilder builder(url, upload_factory_);
  auto status = SetupBuilder(builder, request, "POST");
  if (not status.ok()) {
    return std::make_pair(status,
                          std::unique_ptr<ObjectWriteStreambuf>(nullptr));
  }

  // Set the content type of a sensible value, the application can override this
  // in the options for the request.
  if (not request.HasOption<ContentType>()) {
    builder.AddHeader("content-type: application/octet-stream");
  }
  builder.AddQueryParameter("uploadType", "media");
  builder.AddQueryParameter("name", request.object_name());
  std::unique_ptr<internal::CurlWriteStreambuf> buf(
      new internal::CurlWriteStreambuf(builder.BuildUpload(),
                                       client_options().upload_buffer_size(),
                                       CreateHashValidator(request)));
  return std::make_pair(
      Status(),
      std::unique_ptr<internal::ObjectWriteStreambuf>(std::move(buf)));
}

std::pair<Status, std::unique_ptr<ObjectWriteStreambuf>>
CurlClient::WriteObjectResumable(InsertObjectStreamingRequest const& request) {
  auto session = CreateResumableSessionGeneric(request);
  if (not session.first.ok()) {
    return std::make_pair(std::move(session.first),
                          std::unique_ptr<ObjectWriteStreambuf>(nullptr));
  }

  auto buf =
      google::cloud::internal::make_unique<internal::CurlResumableStreambuf>(
          std::move(session.second), client_options().upload_buffer_size(),
          CreateHashValidator(request));
  return std::make_pair(
      Status(),
      std::unique_ptr<internal::ObjectWriteStreambuf>(std::move(buf)));
}

std::pair<Status, std::string> CurlClient::AuthorizationHeader(
    std::shared_ptr<google::cloud::storage::oauth2::Credentials> const&
        credentials) {
  return credentials->AuthorizationHeader();
}

}  // namespace internal
}  // namespace STORAGE_CLIENT_NS
}  // namespace storage
}  // namespace cloud
}  // namespace google
