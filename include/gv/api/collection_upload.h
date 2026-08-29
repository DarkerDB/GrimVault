#pragma once

#include <gv/core/http.h>
#include <gv/core/result.h>

#include <string>
#include <string_view>
#include <vector>

namespace gv::api {

core::Result<void> validate_collection_upload (std::string_view method, std::string_view url,
                                               const std::vector<core::http::Header>& headers,
                                               const std::vector<std::string>& allowed_hosts);

}
