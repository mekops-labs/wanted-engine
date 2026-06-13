/* SPDX-License-Identifier: Apache-2.0 */

#define JSON(...) #__VA_ARGS__

JSON({
    "action" : "start",
    "params" : {
        "name" : "supervisor",
        "console" : {
            "in" : {"name" : "platform"},
            "out" : {"name" : "platform"},
            "err" : {"name" : "platform"}
        },
        "drivers" : [
            {
                "name" : "socket",
                "path" : "/net/s",
                "options" : "t localhost 8888"
            },
            {"name" : "wanted", "path" : "/dev/wanted"}
        ],
        "preopens" : ["/var/lib/sheriff"]
    }
})
