/*
  ============================================================
   certs.h  -- PUBLIC key material (safe to commit to git)
  ============================================================

  WHAT THIS FILE IS FOR
  ---------------------
  Holds the two PUBLIC certificates/keys the firmware needs:

    1. BROKER_ROOT_CA        -> used to verify we're really talking to
                                the correct MQTT broker (TLS server check).
                                For AWS IoT: paste Amazon Root CA 1.
                                For self-hosted EMQX: paste your own Root CA
                                (generated with openssl, see secrets.h notes).

    2. OTA_SIGNING_PUBLIC_KEY -> used to check that a firmware update was
                                signed by US before we flash it. The
                                matching PRIVATE key stays OFFLINE on your
                                build machine and is NEVER on the device.

  WHERE IT IS USED
  ----------------
   * BROKER_ROOT_CA         -> core/mqtt_handler.cpp (TLS to broker)
   * OTA_SIGNING_PUBLIC_KEY -> core/ota_remote.cpp + core/ota_manager.cpp

  Per-device secrets (device certificate + private key + broker endpoint)
  live in src/secrets.h, which is NOT committed.
*/

#pragma once


// ------------------------------------------------------------------
// BROKER_ROOT_CA  (RSA 2048).  PUBLIC.
//
// Paste the Root CA of whichever broker you use:
//
//   AWS IoT Core:
//     Get the official copy from https://www.amazontrust.com/repository/AmazonRootCA1.pem
//
//   Self-hosted EMQX (generate once, keep my_ca.key OFFLINE):
//     openssl genrsa -out my_ca.key 2048
//     openssl req -x509 -new -key my_ca.key -days 3650 -out my_ca.crt
//     --> paste contents of my_ca.crt below
//
// A single wrong character will make the TLS handshake fail.
// ------------------------------------------------------------------
static const char BROKER_ROOT_CA[] = R"EOF(
-----BEGIN CERTIFICATE-----
MIIBpTCCAUugAwIBAgIUUY9isunYI5CEUYeg5ozRMzB5vQwwCgYIKoZIzj0EAwIw
KDEWMBQGA1UEAwwNQUNIQVMtUm9vdC1DQTEOMAwGA1UECgwFQUNIQVMwHhcNMjYw
NjIzMDI0OTMxWhcNMzYwNjIwMDI0OTMxWjAoMRYwFAYDVQQDDA1BQ0hBUy1Sb290
LUNBMQ4wDAYDVQQKDAVBQ0hBUzBZMBMGByqGSM49AgEGCCqGSM49AwEHA0IABBdu
A6ylaurE24MoM0IKt/XSz01kGzayFEOpYPQi9ATdvQNIh+mubk0IUgmv/DlzEmlp
gLAPZ4UmKcJdZQNeHd+jUzBRMB0GA1UdDgQWBBSbuShA/7iPI+7syC7N9dZ0uMME
bDAfBgNVHSMEGDAWgBSbuShA/7iPI+7syC7N9dZ0uMMEbDAPBgNVHRMBAf8EBTAD
AQH/MAoGCCqGSM49BAMCA0gAMEUCIQDc+p+bHGD0ltNDl733gRknr1kWsL2foNBj
0yG3ZPFLAwIgMLUAkEY30Eg036MkcM6fcQg7jqy23uag+FH5/5gQPJ0=
-----END CERTIFICATE-----
)EOF";


// ------------------------------------------------------------------
// OTA firmware-signing PUBLIC key.  PUBLIC (the device only verifies).
//
// HOW TO MAKE YOUR KEY PAIR (do this ONCE, keep the private key safe):
//   openssl genrsa -out ota_private.pem 2048
//   openssl rsa -in ota_private.pem -pubout -out ota_public.pem
// Paste the contents of ota_public.pem below.
//
// HOW TO SIGN A FIRMWARE FILE before publishing it:
//   openssl dgst -sha256 -sign ota_private.pem -out fw.sig firmware.bin
//   openssl base64 -A -in fw.sig            # this base64 string is the
//                                           # "sig" you put in the OTA command
//
// Until you paste a real key, OTA will SAFELY REFUSE every update
// (signature check fails closed -- nothing gets flashed).
// ------------------------------------------------------------------
static const char OTA_SIGNING_PUBLIC_KEY[] = R"EOF(
-----BEGIN PUBLIC KEY-----
MIIBIjANBgkqhkiG9w0BAQEFAAOCAQ8AMIIBCgKCAQEArvdDsPvJFdoQ/0Bt1n+o
ioAsqsyVBcp9ta679nVjTjVW62BjVLZb9pT5Hu+49WHzuSVxsma8sQ4q3S3EjPkQ
43DUCNu8UTydqx7VmK8OzqssAMFYTVmXWQHog5Yne1s+6hHFoezB92nmDQgwY5PZ
X5xo8x9MoPKYkvuuRQ4mU/w7erJPn+otVtnVKXmLx8jCnRsUwcnW77vq3+nDlF+x
p8xRg/zeTIgT8UVU4HmwvHLXwMa8/Da3clgLntQqQlWfCNy3lqX4puWPE4cZMPel
HPXGihhYzt0KnntVqx/neMJHDBgmDx8Lo+I1SKi2z3QbmsvVEUtQg8La0dhUNtxf
8QIDAQAB
-----END PUBLIC KEY-----
)EOF";
