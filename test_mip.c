#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include "common.h"

int main(void)
{
    struct mip_hdr hdr = {
        .dst = 2,
        .src = 1,
        .ttl = 15,
        .sdu_len = 0,
        .sdu_type = MIP_TYPE_PING
    };

    const char *message = "hello";

    uint8_t buffer[512];

    ssize_t pdu_len = mip_build_pdu(
        &hdr,
        (const uint8_t *)message,
        strlen(message) + 1,
        buffer,
        sizeof(buffer)
    );

    if (pdu_len == -1) {
        printf("Failed to build MIP PDU\n");
        return 1;
    }

    struct mip_hdr parsed_hdr;
    uint8_t *parsed_sdu;
    size_t parsed_sdu_len;

    if (mip_parse_pdu(
            buffer,
            pdu_len,
            &parsed_hdr,
            &parsed_sdu,
            &parsed_sdu_len) == -1) {

        printf("Failed to parse MIP PDU\n");
        return 1;
    }

    printf("dst: %u\n", parsed_hdr.dst);
    printf("src: %u\n", parsed_hdr.src);
    printf("ttl: %u\n", parsed_hdr.ttl);
    printf("sdu_len words: %u\n", parsed_hdr.sdu_len);
    printf("sdu_len bytes: %zu\n", parsed_sdu_len);
    printf("sdu_type: %u\n", parsed_hdr.sdu_type);
    printf("payload: %s\n", (char *)parsed_sdu);

    return 0;
}