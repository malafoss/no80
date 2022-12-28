# no80

no80 - The resource effective redirecting http server

No80 is a minimal dockerized http server that only makes redirects temporarily (302) or permanently (301)
to given URLs. Often used to redirect http service users to a https service. A way more simple way to do
http redirects than, for example, nginx. The docker image size is under one megabyte.

## Using container image

Docker repository is available at [https://hub.docker.com/r/malafoss/no80](https://hub.docker.com/r/malafoss/no80).

To run the latest container:

```
<docker|podman> run <DOCKER_OPTIONS> -p <PORT>:80 docker.io/malafoss/no80 <OPTIONS> <URL>
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
```

With _-P_ option, no80 will make permanent http redirects using [301](https://en.wikipedia.org/wiki/HTTP_301) and without _-P_ option using [302](https://en.wikipedia.org/wiki/HTTP_302).

Example 1:

```
podman run -t -i --rm -p 8080:80 docker.io/malafoss/no80 https://example.com
```

Runs no80 http server which will redirect all port 8080 requests to https://example.com.

Example 2:

```
podman run -t -i --rm -p 8080:80 docker.io/malafoss/no80 -a https://example.com
```

Runs no80 http server which will redirect port 8080 requests having request path _/path_ to https://example.com/path.

Example 3:

```
docker run --userns host --network host -t -i --rm docker.io/malafoss/no80 -a https://`hostname -f`
```
or
```
docker run -t -i --rm -p 80:80 docker.io/malafoss/no80 -a https://`hostname -f`
```

Redirect browsers accessing http port 80 to https port 443 on the current host.

Example 4:

```
podman run -t -i --rm -p 8080:80 docker.io/malafoss/no80 -m /match https://siteA/pathA https://siteB/pathB
```

Redirect browsers accessing http port 8080 to https://siteA/pathA if request path matches with _/match_. Otherwise browsers are redirected to https://siteB/pathB.

Example 5:

```
podman run -t -i --rm -p 8080:80 docker.io/malafoss/no80 -m /match https://siteA/pathA -s /starting https://siteB/pathB -r /redirect https://siteC/pathC https://siteD/pathD
```

Redirect exact path _/match_ to https://siteA/pathA.
Redirect paths starting with _/starting_ such as _/starting/mypath_ to https://siteB/pathB.
Redirect paths starting with _/redirect_ such as _/redirect/mypath_ to https://siteC/pathC/mypath.
Otherwise redirect by default to https://siteD/pathD.

Note that multiple _-m_, _-s_ and _-r_ options are allowed and are processed in the given order.

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
