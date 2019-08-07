#include <pxp-agent/modules/download_file.hpp>
#include <pxp-agent/util/bolt_helpers.hpp>
#include <pxp-agent/util/bolt_module.hpp>
#include <pxp-agent/configuration.hpp>
#include <pxp-agent/module.hpp>
#include <boost/algorithm/hex.hpp>

#define LEATHERMAN_LOGGING_NAMESPACE "puppetlabs.pxp_agent.module.download_file"
#include <leatherman/logging/logging.hpp>
#include <leatherman/file_util/file.hpp>
#include <leatherman/file_util/directory.hpp>

#include <vector>
#include <string>

namespace lth_jc   = leatherman::json_container;
namespace lth_loc  = leatherman::locale;
namespace lth_file = leatherman::file_util;
namespace pcp_util = PCPClient::Util;
namespace fs       = boost::filesystem;

namespace PXPAgent {
namespace Modules {

  static const std::string DOWNLOAD_FILE_ACTION { "download" };

  static const std::string DOWNLOAD_FILE_ACTION_INPUT_SCHEMA { R"(
  {
    "type": "object",
    "properties": {
      "files": {
        "type": "array",
        "items": {
          "type": "object",
          "properties": {
            "destination": {
              "type": "string"
            },
            "link_source": {
              "type":"string"
            },
            "uri": {
              "type": "object",
              "properties": {
                "path": {
                  "type": "string"
                },
                "params": {
                  "type": "object"
                }
              },
              "required": ["path", "params"]
            },
            "sha256": {
              "type": "string"
            },
            "kind": {
              "type": "string"
            }
          },
          "required": ["link_source", "destination", "uri", "sha256", "kind"]
        }
      }
    }
  }
  )" };


  DownloadFile::DownloadFile(const std::vector<std::string>& master_uris,
                             const std::string& ca,
                             const std::string& crt,
                             const std::string& key,
                             const std::string& proxy,
                             uint32_t download_connect_timeout,
                             uint32_t download_timeout,
                             std::shared_ptr<ModuleCacheDir> module_cache_dir,
                             std::shared_ptr<ResultsStorage> storage) :
    BoltModule { "", std::move(storage), std::move(module_cache_dir) },
    Purgeable { module_cache_dir_->purge_ttl_ },
    master_uris_ { master_uris },
    file_download_connect_timeout_ { download_connect_timeout },
    file_download_timeout_ { download_timeout }
  {
    module_name = "download_file";
    actions.push_back(DOWNLOAD_FILE_ACTION);

    PCPClient::Schema input_schema { DOWNLOAD_FILE_ACTION, lth_jc::JsonContainer { DOWNLOAD_FILE_ACTION_INPUT_SCHEMA } };
    PCPClient::Schema output_schema { DOWNLOAD_FILE_ACTION };

    input_validator_.registerSchema(input_schema);
    results_validator_.registerSchema(output_schema);

    client_.set_ca_cert(ca);
    client_.set_client_cert(crt, key);
    client_.set_supported_protocols(CURLPROTO_HTTPS);
    client_.set_proxy(proxy);
  }

  // DownloadFile overrides callAction from the base BoltModule class since there's no need to run
  // any commands with DownloadFile. CallAction will simply download the file and return a result
  // based on if the download succeeded or failed.
  ActionResponse DownloadFile::callAction(const ActionRequest& request)
  {
    auto file_params = request.params();
    auto files = file_params.get<std::vector<lth_jc::JsonContainer>>("files");
    const fs::path& results_dir = request.resultsDir();

    ActionResponse response { ModuleType::Internal, request };
    lth_jc::JsonContainer result;
    for (auto this_file : files) {
      auto destination = fs::path(this_file.get<std::string>("destination"));
      auto kind = this_file.get<std::string>("kind");
      if (kind == "file") {
        try {
          Util::downloadFileFromMaster(master_uris_,
                        file_download_connect_timeout_,
                        file_download_timeout_,
                        client_,
                        module_cache_dir_->createCacheDir(this_file.get<std::string>("sha256")),
                        destination,
                        this_file);
        } catch (Module::ProcessingError& e) {
          return failure_response(request, results_dir, lth_loc::format("Failed to download {1}; {2}", destination, e.what()));
        }
      } else if (kind == "directory"){
        if (fs::exists(destination)) {
          if (!fs::is_directory(destination)) {
            return failure_response(request, results_dir, lth_loc::format("Destination {1} already exists and is not a directory!", destination));
          }
        } else {
          try {
            Util::createDir(destination);
          } catch (fs::filesystem_error& e) {
            return failure_response(request, results_dir, lth_loc::format("Failed to create directory {1}; {2}", destination, e.what()));
          }
        }
      } else if (kind == "symlink") {
        if (fs::exists(destination)) {
          if (!fs::is_symlink(destination)) {
            return failure_response(request, results_dir, lth_loc::format("Destination {1} already exists and is not a symlink!", destination));
          }
        } else {
          Util::createSymLink(fs::path(this_file.get<std::string>("link_source")), destination);
        }
      } else {
          return failure_response(request, results_dir, lth_loc::format("Not a valid file type! {1}", kind));
      }
    }
    response.output = write_results_to_files(results_dir, EXIT_SUCCESS, "downloaded", "");
    processOutputAndUpdateMetadata(response);
    return response;
  }


  unsigned int DownloadFile::purge(
      const std::string& ttl,
      std::vector<std::string> ongoing_transactions,
      std::function<void(const std::string& dir_path)> purge_callback)
  {
      if (purge_callback == nullptr)
        purge_callback = &Purgeable::defaultDirPurgeCallback;
      return module_cache_dir_->purgeCache(ttl, ongoing_transactions, purge_callback);
  }

}  // namespace Modules
}  // namespace PXPAgent
