# no80

no80 - The resource effective redirecting http server

No80 is a minimal dockerized http server that only makes redirects temporarily (302) or permanently (301)
to a given URL. Often used to redirect http service users to a https service. A way more simple way to do
http redirects than, for example, nginx. The docker image size is under one megabyte.

## Using container image

Docker repository is available at [https://hub.docker.com/r/malafoss/no80](https://hub.docker.com/r/malafoss/no80).

To run the latest container:

`<docker|podman> run <DOCKER_OPTIONS> -p <PORT>:80 docker.io/malafoss/no80 <OPTIONS> <URL>`

Options:
```
  -a    Append path from the http request to the redirected URL
  -h    Print this help text and exit
  -p N  Use specified port number N (default is port 80)
  -P    Redirect permanently using 301 instead of temporarily using 302
```

Example 1:

`podman run -t -i --rm -p 8080:80 docker.io/malafoss/no80 https://example.com`

Runs no80 http server which will redirect all port 8080 requests to https://example.com.

Example 2:

`podman run -t -i --rm -p 8080:80 docker.io/malafoss/no80 -a https://example.com`

Runs no80 http server which will redirect port 8080 requests having request path _/path_ to https://example.com/path.

With _-P_ option, no80 will make permanent http redirects using [301](https://en.wikipedia.org/wiki/HTTP_301) and without _-P_ option using [302](https://en.wikipedia.org/wiki/HTTP_302).

Example 3:

```
docker run --userns host --network host --privileged -t -i --rm docker.io/malafoss/no80 -a https://`hostname -f`
```

Redirect browsers accessing http port 80 to https port 443 on the current host.

## How to build?

To build: `./build.sh`

To run tests: `./test.sh`

## How to run locally built image?

Running using docker (or similarly podman): 

```docker run -t -i --rm -p 8080:80 no80 https://example.com```

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
ExecStart=/usr/bin/docker run --rm --name no80 -p 80:80 docker.io/malafoss/no80 https://example.com
ExecStop=/usr/bin/docker stop no80
[Install]
WantedBy=multi-user.target
```

## License

Copyright (c) 2022 Mikko Ala-Fossi

Licensed under [MIT license](LICENSE)
