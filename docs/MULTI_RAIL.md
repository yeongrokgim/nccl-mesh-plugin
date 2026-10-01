# Comparing PCIe and physical-cable grouping

`NCCL_MESH_RAIL_GROUPING` selects how RDMA devices become NCCL-visible
rails. Its default is `pcie`:

```text
pcie rail 0: rocep1s0f0, rocep1s0f1
pcie rail 1: roceP2p1s0f0, roceP2p1s0f1
```

`NCCL_MESH_RAIL_GROUPING=cable` groups matching trailing `fN` port names
across PCIe endpoints:

```text
cable rail 0: rocep1s0f0, roceP2p1s0f0
cable rail 1: rocep1s0f1, roceP2p1s0f1
```

On this machine the trailing `f0`/`f1` convention identifies the corresponding
physical port. If that suffix is unavailable, Mesh falls back to the final PCI
function number. This is a hardware-layout convention; Linux does not provide
a general API that identifies which netdevs share a cable.

A Mesh connection selects one reachable NIC within its requested rail. Cable
mode does not stripe one NCCL connection over every NIC in the cable rail.
Consequently it is useful for measuring the effect of presenting the shared
physical connection as one NCCL device rather than two PCIe devices.

Mesh groups PCI functions belonging to the same PCI bus/device into one
NCCL-visible rail. For the supplied layout, the two pci0000 ports form one
rail and the two pci0002 ports form another. Subnet matching chooses the
appropriate port within each rail. Connections first honor the local NCCL
rail and the listener's preferred rail; subnet fallback remains available
when the requested rail has no direct path.

Each exposed rail reports a distinct GUID. NCCL accounts equal GUID/port
pairs as sharing bandwidth, so reporting GUID zero for every rail can
incorrectly combine their budgets. The v9 name and PCI path storage is also
separate for each device.

Build with `make`. Build the same revision on both hosts and ensure both
hosts have the resulting library at the path below. This command explicitly
adds the build directory to the library search path, preserves the patched
NCCL directory, and corrects `NCCL_MIM_NCHANNELS` to `NCCL_MIN_NCHANNELS`:

```sh
mpirun \
  --host '192.168.1.200:1,192.168.1.203:1' \
  --mca plm_rsh_args '-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null' \
  --mca btl_tcp_if_include enP7s7 \
  --mca oob_tcp_if_include enP7s7 \
  -x LD_LIBRARY_PATH=/home/ygim/git/nccl-mesh-plugin:/home/ygim/nccl-patched \
  -x NCCL_NET_PLUGIN=mesh \
  -x NCCL_NET=Mesh \
  -x NCCL_IB_HCA=rocep1s0f0,roceP2p1s0f0 \
  -x NCCL_MESH_RAIL_GROUPING=pcie \
  -x NCCL_CROSS_NIC=1 \
  -x NCCL_IB_MERGE_NICS=0 \
  -x NCCL_MIN_NCHANNELS=2 \
  -x NCCL_MAX_NCHANNELS=2 \
  -x NCCL_MESH_DEBUG=2 \
  -x NCCL_DEBUG=INFO \
  -x NCCL_DEBUG_SUBSYS=ALL \
  -x NCCL_DEBUG_FILE=/home/ygim/.log/mesh_rails.%h.%p.log \
  -x NCCL_SOCKET_FAMILY=AF_INET \
  -x NCCL_SOCKET_IFNAME='=enP7s7' \
  -- /home/ygim/git/nccl-tests/build/all_reduce_perf \
  -g 1 -t 1 -b 1G -e 1G
```

Run the same command again with:

```sh
-x NCCL_MESH_RAIL_GROUPING=cable
```

For the shown two-host `f0` test and two-device HCA filter, startup should
report two PCIe rails in `pcie` mode and one cable rail in `cable` mode. Keep
message size, channel settings, and HCA filtering identical when comparing
throughput. Set the variable on every rank; different grouping on peers makes
the advertised preferred-rail indices inconsistent.

Use the same message size for comparisons with the fast baseline. Mesh uses
`NCCL_MESH_HCA` when set, otherwise `NCCL_IB_HCA`. Remove the two-device filter
(or list all four devices) for the four-node topology. The opposite nodes in
that ring do not share a direct subnet; rail selection does not itself add
RDMA forwarding between them.

In `pcie` mode, verify startup reports two NCCL-visible PCIe rails. Connection diagnostics
should include `Honoring NCCL rail 0` and `Honoring NCCL rail 1`, with distinct
local RoCE devices. Check channel send and receive assignments for both
`NET/Mesh/0` and `NET/Mesh/1`. Plugin diagnostics may appear in process stderr
as well as NCCL's log file. Two channels allow both rails to be used but do
not guarantee a particular NCCL topology assignment or throughput.

`make test_rails && ./tests/test_rails` tests the production grouping,
properties, HCA filtering and address selection using the supplied four-node
addresses, without RDMA hardware. A multi-host benchmark is still needed to
measure bandwidth; the build and regression tests do not establish parity
with NCCL's built-in IB transport.
