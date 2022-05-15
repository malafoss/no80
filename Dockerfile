FROM scratch
COPY no80 /no80
EXPOSE 80
ENTRYPOINT [ "/no80" ]
CMD [ "https://example.com" ]
