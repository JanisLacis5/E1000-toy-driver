# Sending packets to the test NIC

`send_rx_packet.py` needs Linux and Python 3, with no third-party packages.
It sends raw Ethernet frames containing IPv4 and ICMP echo requests, UDP
datagrams, or TCP SYN segments. It does not wait for replies or perform ARP.
The default IP addresses are test values; neither interface needs those IPs
assigned for testing reception at the descriptor/IRQ level.

## From the host

The development e1000 must be attached to the host TAP (for example `tap-test`).
The TAP must be up. QEMU's user-mode networking does not provide this path.
Use the guest test interface's actual MAC address:

```sh
sudo python3 send_rx_packet.py --interface tap-test --dst-mac 52:54:00:12:34:57
sudo python3 send_rx_packet.py --interface tap-test --dst-mac 52:54:00:12:34:57 --protocol udp --count 5
sudo python3 send_rx_packet.py --interface tap-test --dst-mac 52:54:00:12:34:57 --protocol tcp
```

If testing the additional NIC hot-added with MAC `52:54:00:12:34:58`, use that
address instead. No static neighbor entry is needed with this script.

## From inside the VM

Use a **different, working sender NIC** connected through QEMU's network to
the test e1000. Pass the sender's interface name and the test NIC's MAC:

```sh
sudo python3 send_rx_packet.py --interface <sender-interface> --dst-mac <test-nic-mac>
```

Sending on the test NIC itself exercises its transmit path, not its receive
path. Two separate `-netdev user` backends are not connected to one another.
A veth pair alone also bypasses the test NIC. Network namespaces are optional
for this raw Ethernet sender: it bypasses IP routing/local delivery.

## Inspecting results

Use `--dry-run` to print frame hex without transmitting (no root required).
Use `--payload`, `--src-ip`, and `--dst-ip` to customize packet contents.
UDP and TCP use source port 40000 and destination port 9000.

On the receiving VM, watch `sudo dmesg -w` for the driver's IRQ message and
inspect descriptor completion when available. A successful send only means
the sending kernel accepted the frame; it does not prove the test NIC accepted
it or generated an interrupt. Multiple packets may share an interrupt.
Repeated RX testing needs the driver to acknowledge interrupts and replenish
descriptors. The script neither requires nor checks successful ping replies,
TCP connections, or delivery to the guest networking stack.
