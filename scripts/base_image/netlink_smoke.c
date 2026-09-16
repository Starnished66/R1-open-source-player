#include <stdio.h>
#include <netlink/netlink.h>
#include <netlink/socket.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>

/* Read-only queries to the running kernel; no interface/configuration writes. */
int main(void) {
    struct nl_sock *socket = nl_socket_alloc();
    if (!socket) return 1;
    int result = genl_connect(socket);
    if (result < 0) {
        fprintf(stderr, "genl_connect: %s\n", nl_geterror(result));
        nl_socket_free(socket);
        return 1;
    }
    int control = genl_ctrl_resolve(socket, "nlctrl");
    int wifi = genl_ctrl_resolve(socket, "nl80211");
    printf("Generic netlink: nlctrl=%d, nl80211=%d\n", control, wifi);
    nl_socket_free(socket);
    return control > 0 && wifi > 0 ? 0 : 1;
}
