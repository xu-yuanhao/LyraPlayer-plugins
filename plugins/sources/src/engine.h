#pragma once
#include <functional>
#include <string>
#include <string_view>

namespace sources {
using Transport = std::function<std::string(std::string_view url,std::string_view method,
                                            std::string_view headers,std::string_view body)>;
std::string evaluate(std::string_view script,std::string_view operation,
                     std::string_view arguments,Transport transport = {});
}
