#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "common.h"

int main(void)
{
    struct mip_arp_msg msg = {
        .type = MIP_ARP_REQUEST,
        .address = 5
    };

    uint32_t raw = mip_arp_serialize(&msg);

    struct mip_arp_msg parsed;

    mip_arp_parse(raw, &parsed);

    printf("ARP type: %u\n", parsed.type);
    printf("ARP address: %u\n", parsed.address);

    struct mip_arp_cache cache;
    mip_arp_cache_init(&cache);

    uint8_t mac[6] = {
        0x00, 0x11, 0x22,
        0x33, 0x44, 0x55
    };

    mip_arp_update(&cache, 5, mac, 3);

    struct mip_arp_entry *entry =
        mip_arp_lookup(&cache, 5);

    if (entry == NULL) {
        printf("Cache lookup failed\n");
        return 1;
    }

    printf("Cache MIP: %u\n", entry->mip_addr);
    printf("Cache MAC: ");

    print_mac_addr(entry->mac_addr);

    printf("\nCache ifindex: %d\n", entry->ifindex);

    return 0;
}