# no80

no80 - the resource effective redirecting http server

No80 is a minimal dockerized http server that only makes redirects temporarily (302) or permanently (301)
to a given URL. Often used to redirect http service users to a https service. A way more simple way to do
http redirects than, for example, nginx. The docker image size is under one megabyte.

## Using container image

Docker repository is available at [https://registry.hub.docker.com/r/malafoss/no80](https://registry.hub.docker.com/r/malafoss/no80).

To run the latest container:

`docker run <docker_options> -p <port>:80 docker.io/malafoss/no80 <redirect|permadirect> <url>`

For example,

`docker run -t -i --rm -p 8080:80 docker.io/malafoss/no80 redirect https://example.com`

to run no80 http server in order to redirect port 8080 to https://example.com.

Command _redirect_ makes temporary http redirect using [302](https://en.wikipedia.org/wiki/HTTP_302) and 
command _permadirect_ makes permanent http redirect using [301](https://en.wikipedia.org/wiki/HTTP_301). 

## How to build?

To build: `./build.sh`

To run tests: `./test.sh`

## How to run locally built image?

Running using docker (or similarly podman) to redirect temporarily (302) port 8080 to https://example.com: 

```docker run -t -i --rm -p 8080:80 no80 redirect https://example.com```

Running using docker (or similarly podman) to redirect permanently (301) port 8080 to https://example.com:

```docker run -t -i --rm -p 8080:80 no80 permadirect https://example.com```

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
ExecStart=/usr/bin/docker run --rm --name no80 -p 80:80 docker.io/malafoss/no80 redirect https://example.com
ExecStop=/usr/bin/docker stop no80
[Install]
WantedBy=multi-user.target
```

## License

Copyright (c) 2022 Mikko Ala-Fossi

Licensed under [MIT license](LICENSE)
