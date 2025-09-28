# no80

no80 - The resource effective redirecting http/https server

No80 is a minimal dockerized http and https server that makes redirects temporarily (302) or permanently (301) to given URLs. A way more simple way to do http and https redirects than, for example, nginx.
The docker image size is under 2MB and it should run fine with 32MB of memory (recommended safe limit, probably 16MB is fine).

It runs both HTTP and HTTPS server simultaneously, with given SSL certificates, or by default with generated self-signed certificates.

## Using container image

Docker repository is available at [https://hub.docker.com/r/malafoss/no80](https://hub.docker.com/r/malafoss/no80).

To run the latest container:

```
<docker|podman> run --memory=32m <DOCKER_OPTIONS> -p <PORT>:80 -p <HTTPS_PORT>:443  -v <CERTIFICATE>.pem:/certs/cert.pem -v <CERTIFICATE_KEY>.pem:/certs/key.pem docker.io/malafoss/no80 <OPTIONS> <URL>
```

Options:
```
  -a           Append path from the http request to the redirected URL
  -h           Print this help text and exit
  -m PATH URL  Redirect path matching with PATH to URL
  -s PATH URL  Redirect path starting with PATH to URL
  -r PATH URL  Redirect path starting with PATH to URL appended with the rest of the path
  -p N         Use specified port number N (default is port 80)
  -P           Redirect permanently using 301 instead of temporarily using 302
  -q           Suppress statistics
  -S N         Use specified HTTPS port number N (default is port 443)
  -c FILE      SSL certificate file in PEM format (default: /certs/cert.pem)
  -k FILE      SSL private key file in PEM format (default: /certs/key.pem)
  -v           Print version information and exit
```

With _-P_ option, no80 will make permanent http redirects using [301](https://en.wikipedia.org/wiki/HTTP_301) and without _-P_ option using [302](https://en.wikipedia.org/wiki/HTTP_302).

Example 1:

```
podman run --memory=32m -t -i --rm -p 8080:80 -p 8443:443 -v ./certs/cert.pem:/certs/cert.pem -v ./certs/key.pem:/certs/key.pem docker.io/malafoss/no80 https://example.com
```

Runs no80 HTTP server on port 8080 and HTTPS server on port 8443 with given certificate and certificate key. Both servers will redirect requests to https://example.com.

Example 2:

```
podman run --memory=32m -t -i --rm -p 8080:80 docker.io/malafoss/no80 -a https://example.com
```

Runs no80 http server which will redirect port 8080 requests having request path _/path_ to https://example.com/path.

Example 3:

```
docker run --memory=32m --userns host --network host -t -i --rm docker.io/malafoss/no80 -a https://`hostname -f`
```
or
```
docker run --memory=32m -t -i --rm -p 80:80 docker.io/malafoss/no80 -a https://`hostname -f`
```

Redirect browsers accessing http port 80 to https port 443 on the current host.

Example 4:

```
podman run --memory=32m -t -i --rm -p 8080:80 docker.io/malafoss/no80 -m /match https://siteA/pathA https://siteB/pathB
```

Redirect browsers accessing http port 8080 to https://siteA/pathA if request path matches with _/match_. Otherwise browsers are redirected to https://siteB/pathB.

Example 5:

```
podman run --memory=32m -t -i --rm -p 8080:80 docker.io/malafoss/no80 -m /match https://siteA/pathA -s /starting https://siteB/pathB -r /redirect https://siteC/pathC https://siteD/pathD
```

Redirect exact path _/match_ to https://siteA/pathA.
Redirect paths starting with _/starting_ such as _/starting/mypath_ to https://siteB/pathB.
Redirect paths starting with _/redirect_ such as _/redirect/mypath_ to https://siteC/pathC/mypath.
Otherwise redirect by default to https://siteD/pathD.

Note that multiple _-m_, _-s_ and _-r_ options are allowed and are processed in the given order.

Example 6:

```
docker run --memory=32m -t -i --rm -p 80:80 -p 443:443 -v /path/to/certs:/certs docker.io/malafoss/no80 -c /certs/cert.pem -k /certs/key.pem https://example.com
```

Runs both HTTP server on port 80 and HTTPS server on port 443 simultaneously. Both servers will redirect requests to https://example.com. The SSL certificate and key files are mounted from the host system.

Example 7:

```
docker run --memory=32m -t -i --rm -p 80:80 -p 443:443 docker.io/malafoss/no80 https://example.com
```

Runs both HTTP server on port 80 and HTTPS server on port 443 simultaneously using the default self-signed certificates included in the image. Both servers will redirect requests to https://example.com.

## SSL/TLS and Cipher Configuration

no80 uses WolfSSL for HTTPS support with the following configuration:

**TLS Versions**: TLS 1.3 (preferred) with automatic fallback to TLS 1.2 minimum
**SSL/TLS Optimizations**: Hardware acceleration (Intel AES-NI, AVX), assembly optimizations

**Cipher Suites** (in order of preference):
- `TLS13-AES128-GCM-SHA256` - TLS 1.3 with AES-128-GCM (fastest)
- `TLS13-CHACHA20-POLY1305-SHA256` - TLS 1.3 with ChaCha20-Poly1305
- `ECDHE-RSA-AES128-GCM-SHA256` - TLS 1.2 with ECDHE and AES-128-GCM
- `ECDHE-RSA-CHACHA20-POLY1305` - TLS 1.2 with ECDHE and ChaCha20-Poly1305

The cipher selection prioritizes performance while maintaining strong security. AES-128-GCM is preferred for hardware-accelerated systems, while ChaCha20-Poly1305 provides excellent performance on systems without AES hardware acceleration.

## How to build?

To build: `./build.sh`

To run tests: `./test.sh`

## How to run locally built image?

Running using docker (or similarly podman): 

```docker run --memory=32m -t -i --rm -p 8080:80 -p 8443:443 no80 https://example.com```

## How to run using systemd?

Running using systemd:

_/etc/systemd/system/no80.service_
```
[Unit]
Description=no80 http service
After=docker.service
Requires=docker.service
[Service]
TimeoutStartSec=0
Restart=always
RestartSec=3
ExecStartPre=-/usr/bin/docker stop no80
ExecStartPre=-/usr/bin/docker rm no80
ExecStartPre=/usr/bin/docker pull docker.io/malafoss/no80
ExecStart=/usr/bin/docker run --memory=32m --rm --name no80 -p 80:80 -p 443:443 docker.io/malafoss/no80 https://example.com
ExecStop=/usr/bin/docker stop no80
[Install]
WantedBy=multi-user.target
```

## License

Copyright (c) 2025 Mikko Ala-Fossi

Licensed under [MIT license](LICENSE)
