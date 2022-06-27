#define STRINGIFY(...) #__VA_ARGS__

STRINGIFY(
{
    "action": "start",
    "params": {
        "name": "supervisor",
        "console": {
            "in":  {"name": "platform"},
            "out": {"name": "platform"},
            "err": {"name": "platform"}
        },
        "drivers": [
            {
                "name": "rom",
                "path": "/rom"
            },
            {
                "name": "platform",
                "path": "/mnt",
                "options": "./"
            },
            {
                "name": "virt",
                "path": "/net"
            },
            {
                "name": "socket",
                "path": "/net/s",
                "options": "t localhost 8888"
            },
            {
                "name": "socket",
                "path": "/net/ss",
                "options": "T localhost 8889"
            },
            {
                "name": "virt",
                "path": "/d"
            },
            {
                "name": "9p",
                "path": "/d/b",
                "options": "tcp!localhost!5640"
            },
            {
                "name": "wanted",
                "path": "/w"
            }
        ]
    }
}
)
