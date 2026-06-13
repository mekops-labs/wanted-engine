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
            {"name" : "wanted"}
        ],
        "mounts" : [
            {"name" : "platform", "path" : "/var/lib/sheriff"}
        ],
        "sockets" : [
            {"name" : "s", "address" : "tcp://localhost:8888"}
        ]
    }
})
