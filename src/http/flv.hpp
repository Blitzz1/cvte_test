#pragma once
#include "app/types.hpp"
#include <httplib.h>
namespace avqm {
void install_flv_route(httplib::Server &server, Application &app);
}
