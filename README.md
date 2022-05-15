# no80

no80 - the resource effective redirecting http server

Dockerized minimal http server that only redirects temporarily (302) or permanently (301) to a given URL.

## How to build?

Run: `./build.sh`

## How to run?

Running using docker (or similarly podman) to redirect temporarily (302) port 8080 to https://example.com: 

```docker run -t -i --rm -p 8080:80 no80 redirect https://example.com```

Running using docker (or similarly podman) to redirect permanently (301) port 8080 to https://example.com:

```docker run -t -i --rm -p 8080:80 no80 redirect https://example.com```

## License

Copyright (c) 2022 Mikko Ala-Fossi

Licensed under MIT license
