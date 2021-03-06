/*
 * Copyright (C) 2014 Cloudius Systems, Ltd.
 *
 * This work is open source software, licensed under the terms of the
 * BSD license as described in the LICENSE file in the top-level directory.
 */

#ifndef SERVER_MODULE_HH_
#define SERVER_MODULE_HH_

#include "cloud-init.hh"
#include "global_server.hh"
#include "files-module.hh"

namespace init {

class server_module : public config_module
{
    virtual void handle(const YAML::Node& doc) override
    {
        std::stringstream s;
        for (auto& node : doc) {
            s << node.first.as<std::string>() <<": " << node.second.as<std::string>() << std::endl;
        }
        files_module::create_file("/tmp/httpserver.conf", s.str());
    }

    virtual std::string get_label() override
    {
        return "httpserver";
    }
};
}

#endif /* SERVER_MODULE_HH_ */
