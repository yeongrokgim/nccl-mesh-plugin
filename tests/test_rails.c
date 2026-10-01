/* Test the actual plugin helpers without opening devices or starting threads. */
#include <assert.h>
#include "../src/mesh_plugin.c"

static const uint32_t ips[4][4] = {
    {0x0a0a2801, 0x0a0a0a01, 0x0a0a2805, 0x0a0a0a05},
    {0x0a0a1402, 0x0a0a0a02, 0x0a0a1406, 0x0a0a0a06},
    {0x0a0a1403, 0x0a0a1e03, 0x0a0a1407, 0x0a0a1e07},
    {0x0a0a2804, 0x0a0a1e04, 0x0a0a2808, 0x0a0a1e08}
};

static void setup(int node, int grouping) {
    memset(&g_mesh_state, 0, sizeof(g_mesh_state));
    g_mesh_state.num_nics = 4;
    g_mesh_state.rail_grouping = grouping;
    for (int i = 0; i < 4; i++) {
        struct mesh_nic *n = &g_mesh_state.nics[i];
        snprintf(n->dev_name, sizeof(n->dev_name), "roce%sf%d",
                 i < 2 ? "p1" : "P2", i % 2);
        snprintf(n->pci_path, sizeof(n->pci_path),
                 "/sys/devices/pci%04x:00/%04x:01:00.%d", i / 2, i / 2, i % 2);
        n->ip_addr = ips[node][i];
        n->netmask = 0xffffff00;
        n->subnet = n->ip_addr & n->netmask;
        n->port_num = 1;
    }
    assert(mesh_build_rails() == 0);
    assert(mesh_net_device_count() == 2);
    assert(g_mesh_state.rails[0].num_nics == 2);
    assert(g_mesh_state.rails[1].num_nics == 2);
    if (grouping == MESH_RAIL_GROUP_PCIE) {
        assert(g_mesh_state.rails[0].nic_indices[0] == 0);
        assert(g_mesh_state.rails[0].nic_indices[1] == 1);
        assert(g_mesh_state.rails[1].nic_indices[0] == 2);
        assert(g_mesh_state.rails[1].nic_indices[1] == 3);
    } else {
        assert(g_mesh_state.rails[0].nic_indices[0] == 0);
        assert(g_mesh_state.rails[0].nic_indices[1] == 2);
        assert(g_mesh_state.rails[1].nic_indices[0] == 1);
        assert(g_mesh_state.rails[1].nic_indices[1] == 3);
    }
}

static int grouped_nic(int grouping, int rail, int member) {
    return grouping == MESH_RAIL_GROUP_PCIE ? rail * 2 + member
                                            : member * 2 + rail;
}

static struct mesh_handle peer_handle(int node, int grouping, int rail) {
    struct mesh_handle h = { .num_addrs = 4, .selected_count = 2 };
    for (int i = 0; i < 4; i++) {
        int group = i < 2 ? rail : 1 - rail;
        int idx = grouped_nic(grouping, group, i % 2);
        h.addrs[i].ip = htonl(ips[node][idx]);
        h.addrs[i].mask = htonl(0xffffff00);
        h.addrs[i].nic_idx = idx;
    }
    return h;
}

static int rail_has_path(int peer, int rail) {
    const struct mesh_rail *r = &g_mesh_state.rails[rail];
    for (int i = 0; i < r->num_nics; i++) {
        const struct mesh_nic *nic = &g_mesh_state.nics[r->nic_indices[i]];
        for (int j = 0; j < 4; j++) {
            if (mesh_nic_reaches_ip(nic, ips[peer][j])) return 1;
        }
    }
    return 0;
}

int main(void) {
    assert(sizeof(struct mesh_handle) <= NCCL_NET_HANDLE_MAXSIZE);
    assert(mesh_hca_filter_allows("rocep1s0f0,roceP2p1s0f0", "roceP2p1s0f0"));
    assert(!mesh_hca_filter_allows("rocep1s0f0,roceP2p1s0f0", "rocep1s0f1"));
    assert(!mesh_hca_filter_allows("^=rocep1s0f0", "rocep1s0f0"));
    assert(mesh_parse_rail_grouping(NULL) == MESH_RAIL_GROUP_PCIE);
    assert(mesh_parse_rail_grouping("PCIE") == MESH_RAIL_GROUP_PCIE);
    assert(mesh_parse_rail_grouping("cable") == MESH_RAIL_GROUP_CABLE);
    assert(mesh_parse_rail_grouping("subnet") == -1);

    for (int grouping = MESH_RAIL_GROUP_PCIE;
         grouping <= MESH_RAIL_GROUP_CABLE; grouping++) {
        for (int node = 0; node < 4; node++) {
            setup(node, grouping);
            for (int peer = 0; peer < 4; peer++) {
                if (peer == node) continue;
                for (int rail = 0; rail < 2; rail++) {
                    struct mesh_handle h = peer_handle(peer, grouping, rail);
                    struct mesh_nic *nic = NULL;
                    struct mesh_addr_entry *addr = mesh_select_rail_address(
                        &g_mesh_state.rails[rail], &h, &nic);
                    int expected = (node + 2) % 4 != peer;
                    if (grouping == MESH_RAIL_GROUP_CABLE && expected) {
                        expected = rail_has_path(peer, rail);
                    }
                    if (!expected) {
                        assert(addr == NULL && nic == NULL);
                    } else {
                        assert(addr && nic);
                        assert(mesh_rail_contains_nic(&g_mesh_state.rails[rail],
                                                     nic - g_mesh_state.nics));
                        assert(mesh_nic_reaches_ip(nic, ntohl(addr->ip)));
                        if (grouping == MESH_RAIL_GROUP_PCIE) {
                            assert(addr->nic_idx / 2 == rail);
                        } else {
                            assert(addr->nic_idx % 2 == rail);
                        }
                    }
                }
            }
        }
    }
    setup(0, MESH_RAIL_GROUP_PCIE);
    ncclNetProperties_v8_t a8, b8;
    ncclNetProperties_v9_t a9, b9;
    assert(mesh_getProperties(0, &a8) == ncclSuccess);
    assert(mesh_getProperties(1, &b8) == ncclSuccess);
    assert(a8.guid != b8.guid);
    assert(mesh_getProperties_v9(0, &a9) == ncclSuccess);
    assert(mesh_getProperties_v9(1, &b9) == ncclSuccess);
    assert(a9.guid == a8.guid && b9.guid == b8.guid);
    assert(strcmp(a9.name, "MeshRail0") == 0);
    assert(strcmp(a9.pciPath, b9.pciPath) != 0);
    assert(mesh_getProperties_v9(2, &b9) == ncclInvalidArgument);
    struct mesh_handle h = peer_handle(3, MESH_RAIL_GROUP_PCIE, 0);
    struct mesh_nic *nic;
    /* A differing peer rail preference must preserve the local rail. */
    assert(mesh_select_rail_address(&g_mesh_state.rails[1], &h, &nic));
    assert(nic == &g_mesh_state.nics[2]);
    /* Legacy handles have no preferred range. */
    h.selected_count = 0;
    assert(mesh_select_rail_address(&g_mesh_state.rails[1], &h, &nic));
    assert(nic == &g_mesh_state.nics[2]);
    h.num_addrs = MESH_MAX_ADDRS + 1;
    assert(!mesh_select_rail_address(&g_mesh_state.rails[1], &h, &nic));
    assert(nic == NULL);
    setup(0, MESH_RAIL_GROUP_CABLE);
    assert(strcmp(g_mesh_state.rails[0].key, "cable:port:0") == 0);
    assert(strcmp(g_mesh_state.rails[1].key, "cable:port:1") == 0);

    /* The two-device f0 benchmark filter collapses to one cable rail. */
    g_mesh_state.nics[1] = g_mesh_state.nics[2];
    g_mesh_state.num_nics = 2;
    assert(mesh_build_rails() == 0);
    assert(g_mesh_state.num_rails == 1);
    assert(g_mesh_state.rails[0].num_nics == 2);

    puts("PCIe/cable grouping, properties, filters and subnet selection passed");
    return 0;
}
