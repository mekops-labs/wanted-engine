/* SPDX-License-Identifier: Apache-2.0 */

#define STRINGIFY(...) #__VA_ARGS__

STRINGIFY({
    "action" : "start",
    "params" : {
        "name" : "app1",
        "console" : {
            "in" : {"name" : "platform", "options" : ""},
            "out" : {"name" : "platform", "options" : ""},
            "err" : {"name" : "platform", "options" : ""}
        },
        "drivers" : [
            {"name" : "rom", "path" : "/rom", "options" : ""},
            {"name" : "platform", "path" : "/mnt", "options" : "./"},
            {"name" : "rom", "path" : "/rom", "options" : ""},
            {"name" : "socket", "path" : "/s", "options" : "t 127.0.0.1 8888"},
            {"name" : "wanted", "path" : "/w", "options" : ""}
        ]
    }
})
