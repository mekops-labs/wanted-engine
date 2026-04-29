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
            {
                "name" : "socket",
                "path" : "/net/ss",
                "options" : "T localhost 8889"
            },
            {"name" : "9p", "path" : "/dev/9p", "options" : "tcp!localhost!5640"},
            {"name" : "wanted", "path" : "/dev/wanted"},
            {"name" : "config", "path" : "/dev/config",
                "options" : "{\"config_file\":\"/config.json\"}"
            }
        ]
    }
})
