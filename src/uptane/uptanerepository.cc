#include "uptane/uptanerepository.h"

#include <openssl/bio.h>
#include <openssl/pem.h>
#include <openssl/x509.h>
#include <boost/algorithm/hex.hpp>
#include <boost/algorithm/string/replace.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/make_shared.hpp>

#include <stdio.h>

#include "bootstrap.h"
#include "crypto.h"
#include "logger.h"
#include "openssl_compat.h"
#include "ostree.h"
#include "utils.h"

namespace Uptane {

Repository::Repository(const Config &config_in)
    : config(config_in),
      director("director", config.uptane.director_server, config),
      image("repo", config.uptane.repo_server, config),
      http(),
      manifests(Json::arrayValue),
      transport(&secondaries) {
  std::vector<Uptane::SecondaryConfig>::iterator it;
  for (it = config.uptane.secondaries.begin(); it != config.uptane.secondaries.end(); ++it) {
    Secondary s(*it, this);
    addSecondary(it->ecu_serial, it->ecu_hardware_id);
    secondaries.push_back(s);
  }
}

void Repository::updateRoot(Version version) {
  director.updateRoot(version);
  image.updateRoot(version);
}

bool Repository::putManifest() { return putManifest(Json::nullValue); }

bool Repository::putManifest(const Json::Value &custom) {
  Json::Value version_manifest;
  version_manifest["primary_ecu_serial"] = config.uptane.primary_ecu_serial;
  version_manifest["ecu_version_manifest"] = transport.getManifests();

  Json::Value unsigned_ecu_version =
      OstreePackage::getEcu(config.uptane.primary_ecu_serial, config.ostree.sysroot).toEcuVersion(custom);
  Json::Value ecu_version_signed =
      Crypto::signTuf(config.uptane.private_key_path, config.uptane.public_key_path, unsigned_ecu_version);
  version_manifest["ecu_version_manifest"].append(ecu_version_signed);
  Json::Value tuf_signed =
      Crypto::signTuf(config.uptane.private_key_path, config.uptane.public_key_path, version_manifest);
  HttpResponse reponse = http.put(config.uptane.director_server + "/manifest", tuf_signed);
  return reponse.isOk();
}

void Repository::refresh() {
  putManifest();
  director.updateRoot(Version());
  image.updateRoot(Version());
  image.fetchAndCheckRole(Role::Snapshot());
  image.fetchAndCheckRole(Role::Timestamp());
}

std::pair<uint32_t, std::vector<Uptane::Target> > Repository::getTargets() {
  refresh();

  std::pair<uint32_t, std::vector<Uptane::Target> > director_targets_pair = director.fetchTargets();
  std::vector<Uptane::Target> director_targets = director_targets_pair.second;
  uint32_t version = director_targets_pair.first;
  std::vector<Uptane::Target> image_targets = image.fetchTargets().second;
  std::vector<Uptane::Target> primary_targets;
  std::vector<Uptane::Target> secondary_targets;

  if (!director_targets.empty()) {
    for (std::vector<Uptane::Target>::iterator it = director_targets.begin(); it != director_targets.end(); ++it) {
      std::vector<Uptane::Target>::iterator image_target_it;
      image_target_it = std::find(image_targets.begin(), image_targets.end(), *it);
      if (image_target_it != image_targets.end()) {
        image.saveTarget(*image_target_it);
        if (it->ecu_identifier() == config.uptane.primary_ecu_serial) {
          primary_targets.push_back(*it);
        } else {
          secondary_targets.push_back(*it);
        }
      } else {
        throw MissMatchTarget("director and image");
      }
    }
    transport.sendTargets(secondary_targets);
  }
  return std::pair<uint32_t, std::vector<Uptane::Target> >(version, primary_targets);
}

bool Repository::deviceRegister() {
  bool pkey_exists = boost::filesystem::exists(config.tls.certificates_directory / config.tls.pkey_file);
  bool certificate_exists =
      boost::filesystem::exists(config.tls.certificates_directory / config.tls.client_certificate);
  bool ca_exists = boost::filesystem::exists(config.tls.certificates_directory / config.tls.ca_file);

  if (pkey_exists && certificate_exists && ca_exists) {
    LOGGER_LOG(LVL_trace, "Device already registered, proceeding");
    return true;
  }

  if (pkey_exists || certificate_exists || ca_exists) {
    LOGGER_LOG(LVL_error, "Device registration was interrupted nothing can be done");
    return false;
  }

  Bootstrap boot(config.provision.provision_path);
  std::string p12_str = boot.getP12Str();
  if (p12_str.empty()) {
    return false;
  }

  FILE *reg_p12 = fmemopen(const_cast<char *>(p12_str.c_str()), p12_str.size(), "rb");
  if (!Crypto::parseP12(reg_p12, config.provision.p12_password, boot.getPkeyPath(), boot.getCertPath(),
                        boot.getCaPath())) {
    fclose(reg_p12);
    return false;
  }
  fclose(reg_p12);

  http.setCerts(boot.getPkeyPath(), boot.getCertPath(), boot.getCaPath());

  Json::Value data;
  data["deviceId"] = config.uptane.device_id;
  data["ttl"] = config.provision.expiry_days;
  HttpResponse response = http.post(config.tls.server + "/devices", data);
  if (!response.isOk()) {
    LOGGER_LOG(LVL_error, "error tls registering device, response: " << response.body);
    return false;
  }

  FILE *device_p12 = fmemopen(const_cast<char *>(response.body.c_str()), response.body.size(), "rb");
  if (!Crypto::parseP12(device_p12, "", (config.tls.certificates_directory / config.tls.pkey_file).string(),
                        (config.tls.certificates_directory / config.tls.client_certificate).string(),
                        (config.tls.certificates_directory / config.tls.ca_file).string())) {
    return false;
  }
  fclose(device_p12);
  sync();
  LOGGER_LOG(LVL_info, "Provisioned successfully on Device Gateway");
  return true;
}

bool Repository::authenticate() {
  return http.authenticate((config.tls.certificates_directory / config.tls.client_certificate).string(),
                           (config.tls.certificates_directory / config.tls.ca_file).string(),
                           (config.tls.certificates_directory / config.tls.pkey_file).string());
}

bool Repository::ecuRegister() {
  if (!Crypto::generateRSAKeyPairIfMissing(config.uptane.public_key_path, config.uptane.private_key_path)) {
    LOGGER_LOG(LVL_error, "Could not generate rsa keys for primary.");
    return false;
  }

  Json::Value all_ecus;
  all_ecus["primary_ecu_serial"] = config.uptane.primary_ecu_serial;
  all_ecus["ecus"] = Json::Value(Json::arrayValue);

  PublicKey primary_pub_key(config.uptane.public_key_path);
  Json::Value primary_ecu;
  primary_ecu["hardware_identifier"] = config.uptane.primary_ecu_hardware_id;
  primary_ecu["ecu_serial"] = config.uptane.primary_ecu_serial;
  primary_ecu["clientKey"]["keytype"] = "RSA";
  primary_ecu["clientKey"]["keyval"]["public"] = primary_pub_key.value;
  all_ecus["ecus"].append(primary_ecu);
  std::vector<SecondaryConfig>::iterator it;
  for (it = registered_secondaries.begin(); it != registered_secondaries.end(); ++it) {
    std::string pub_path = (config.tls.certificates_directory / (it->ecu_serial + ".pub")).string();
    std::string priv_path = (config.tls.certificates_directory / (it->ecu_serial + ".priv")).string();
    Crypto::generateRSAKeyPairIfMissing(pub_path, priv_path);
    transport.sendPrivateKey(it->ecu_serial, Utils::readFile(priv_path));
    Json::Value ecu;
    ecu["hardware_identifier"] = it->ecu_hardware_id;
    ecu["ecu_serial"] = it->ecu_serial;
    ecu["clientKey"]["keytype"] = "RSA";
    ecu["clientKey"]["keyval"]["public"] = Utils::readFile(pub_path);
    all_ecus["ecus"].append(ecu);
  }

  authenticate();
  sync();  // Ensure that the keys written above are on disk before we talk to the server
  HttpResponse response = http.post(config.tls.server + "/director/ecus", all_ecus);
  if (response.isOk()) {
    LOGGER_LOG(LVL_info, "Provisioned successfully on Director");
  } else {
    Json::Value resp_code = response.getJson()["code"];
    // This doesn't work device_already_registered and ecu_already_registered are possible
    if (resp_code.isString() &&
        (resp_code.asString() == "ecu_already_registered" || resp_code.asString() == "device_already_registered")) {
      LOGGER_LOG(LVL_trace, "ECUs are already registered, proceeding");
      return true;
    } else {
      LOGGER_LOG(LVL_error, "Error registering device on Uptane, response: " << response.body);
      return false;
    }
  }
  return true;
}

void Repository::addSecondary(const std::string &ecu_serial, const std::string &hardware_identifier) {
  SecondaryConfig c;
  c.ecu_serial = ecu_serial;
  c.ecu_hardware_id = hardware_identifier;
  registered_secondaries.push_back(c);
}
}
