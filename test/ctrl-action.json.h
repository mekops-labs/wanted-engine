/* SPDX-License-Identifier: Apache-2.0 */

#define STRINGIFY(...) #__VA_ARGS__

STRINGIFY({
    "action" : "start",
    "params" : {
        "name" : "app1",
        "image" : "app1img",
        "console" : {
            "in" : {"name" : "platform", "options" : ""},
            "out" : {"name" : "platform", "options" : ""},
            "err" : {"name" : "platform", "options" : ""}
        },
        "drivers" : [
            {"name" : "wanted"},
            {"name" : "gpio"}
        ],
        "mounts" : [
            {"name" : "config", "path" : "/etc/config", "options" : ""},
            {"name" : "platform", "path" : "/mnt"}
        ],
        "sockets" : [
            {"name" : "uplink", "address" : "tcp://127.0.0.1:8888"}
        ],
        "args" : ["--verbose", "--port"],
        "envs" : ["TZ=UTC", "LANG=C"]
    }
})
