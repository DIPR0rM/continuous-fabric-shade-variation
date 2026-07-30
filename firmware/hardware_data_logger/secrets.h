#include <pgmspace.h>

#define SECRET_WIFI_SSID ""
#define SECRET_WIFI_PASS ""

// Paste AWS IoT Core Endpoint address here:
#define AWS_IOT_ENDPOINT ""

// 1. Paste Amazon Root CA 1 inside the parenthesis below
static const char AWS_CERT_CA[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----

)EOF";

// 2. Paste Device Certificate (.pem.crt) inside the parenthesis below
static const char AWS_CERT_CRT[] PROGMEM = R"EOF(
-----BEGIN CERTIFICATE-----

-----END CERTIFICATE-----
)EOF";

// 3. Paste Private Key (-private.pem.key) inside the parenthesis below
static const char AWS_CERT_PRIVATE[] PROGMEM = R"EOF(
-----BEGIN RSA PRIVATE KEY-----

-----END RSA PRIVATE KEY-----
)EOF";
