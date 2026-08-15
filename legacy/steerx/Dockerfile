ARG ARCH=aarch64
ARG VERSION=12.2.0
FROM axisecp/acap-native-sdk:${VERSION}-${ARCH}

COPY ./app /opt/app/
WORKDIR /opt/app

# WSL2 mounts /mnt/c as 777, which makes the ACAP installer silently skip
# creating the web-UI/proxy rules on the device. Normalise to Unix perms.
RUN find . -type f -exec chmod 644 {} + && \
    find . -type d -exec chmod 755 {} +

RUN . /opt/axis/acapsdk/environment-setup* && acap-build .
