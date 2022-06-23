# Simple test for secure sockets

Wanted Wapp config:

- Driver used: `socket`
- Options: `T localhost 8889`

Start SSL server:

```bash
# create sample cert and key
openssl req -x509 -newkey rsa:2048 -keyout key.pem -out cert.pem -days 365 -nodes
# start tls server
openssl s_server -key key.pem -cert cert.pem -accept 8889
```