#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "config.h"
#include "json/json.h"
#include "sotahttpclient.h"
#include "types.h"

class HttpClientMock : public HttpClient {
 public:
  MOCK_METHOD1(authenticate, bool(const AuthConfig &conf));
  MOCK_METHOD1(get, Json::Value(const std::string &url));
  MOCK_METHOD2(post,
               Json::Value(const std::string &url, const Json::Value &data));
  MOCK_METHOD2(download,
               bool(const std::string &url, const std::string &filename));
};

class HttpSotaClientTest : public ::testing::Test {
 protected:
  void SetUp() {}

  virtual void TearDown() { usleep(300000); }
};

bool operator==(const AuthConfig &auth1, const AuthConfig &auth2) {
  return (auth1.server == auth2.server && auth1.client_id == auth2.client_id &&
          auth1.client_secret == auth2.client_secret);
}
TEST(AuthenticateTest, authenticate_called) {
  Config conf;
  conf.auth.server = "testserver_test";
  conf.auth.client_id = "client_id_test";
  conf.auth.client_secret = "client_secret_test";
  HttpClientMock http;
  EXPECT_CALL(http, authenticate(conf.auth));
  SotaHttpClient sota_client(conf, &http);
}

TEST(DownloadTest, download_called) {
  Config conf;
  conf.core.server = "http://test.com";
  conf.device.uuid = "test_uuid";
  conf.device.packages_dir = "/tmp/";
  data::UpdateRequestId update_request_id = "testupdateid";

  testing::NiceMock<HttpClientMock> http;
  SotaHttpClient sota_client(conf, &http);

  EXPECT_CALL(
      http, download(conf.core.server + "/api/v1/mydevice/test_uuid/updates/" +
                         update_request_id + "/download",
                     conf.device.packages_dir + update_request_id));
  Json::Value result = sota_client.downloadUpdate(update_request_id);
  EXPECT_EQ(result["updateId"].asString(), update_request_id);
  EXPECT_EQ(result["resultCode"].asInt(), 0);
  EXPECT_EQ(result["resultText"].asString(), "Downloaded");
}

TEST(GetAvailableUpdatesTest, get_performed) {
  Config conf;
  conf.core.server = "http://test.com";
  conf.device.uuid = "test_uuid";

  testing::NiceMock<HttpClientMock> http;
  SotaHttpClient sota_client(conf, &http);

  std::string message(
      "[ "
      "{\n"
      "   \"createdAt\" : \"2017-01-05T08:43:40.685Z\",\n"
      "		\"installPos\" : 0,\n"
      "		\"packageId\" : \n"
      "		{\n"
      "			\"name\" : \"treehub-ota-raspberrypi3\",\n"
      "			\"version\" : "
      "\"15f13e2582f29d2218f88adada52ff043d6e9596b7cea288321ed91e275a1768\"\n"
      "		},\n"
      "		\"requestId\" : \"06d64e46-cb25-4d76-b62e-4341b6944d07\",\n"
      "		\"status\" : \"Pending\",\n"
      "		\"updatedAt\" : \"2017-01-05T08:43:40.685Z\"\n"
      "	}\n"
      "]");

  Json::Reader reader;
  Json::Value return_val;
  reader.parse(message, return_val);

  testing::DefaultValue<Json::Value>::Set(return_val);
  EXPECT_CALL(http, get(conf.core.server + "/api/v1/mydevice/" +
                        conf.device.uuid + "/updates"));
  std::vector<data::UpdateRequest> update_requests =
      sota_client.getAvailableUpdates();
  EXPECT_EQ(update_requests.size(), 1);
  EXPECT_EQ(update_requests[0].packageId.name, "treehub-ota-raspberrypi3");
  EXPECT_EQ(update_requests[0].status, data::UpdateRequestStatus::Pending);
  EXPECT_EQ(update_requests[0].requestId,
            "06d64e46-cb25-4d76-b62e-4341b6944d07");
}

TEST(ReportTest, post_called) {
  Config conf;
  conf.core.server = "http://test.com";
  conf.device.uuid = "test_uuid";
  data::UpdateRequestId update_request_id = "testupdateid";

  data::OperationResult oper_result;
  oper_result.id = "estid";
  oper_result.result_code = data::UpdateResultCode::OK;
  oper_result.result_text = "good";

  std::vector<data::OperationResult> operation_results;
  operation_results.push_back(oper_result);
  data::UpdateReport update_report;
  update_report.update_id = "testupdateid";
  update_report.operation_results = operation_results;

  Json::Reader reader;
  Json::Value return_val;
  reader.parse("{\"status\":\"ok\"}", return_val);
  testing::DefaultValue<Json::Value>::Set(return_val);

  testing::NiceMock<HttpClientMock> http;
  SotaHttpClient sota_client(conf, &http);

  std::string url = conf.core.server + "/api/v1/mydevice/" + conf.device.uuid;
  url += "/updates/" + update_report.update_id;
  EXPECT_CALL(http, post(url, update_report.toJson()["operation_results"]));
  Json::Value result = sota_client.reportUpdateResult(update_report);
  EXPECT_EQ(result["status"].asString(), "ok");
}

#ifndef __NO_MAIN__
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
#endif