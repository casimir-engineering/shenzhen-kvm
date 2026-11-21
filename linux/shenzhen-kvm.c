/* shenzhen-kvm: the Linux half of Shenzhen KVM. One program, kept in parts under src/.
 * Build: gcc -O2 -o shenzhen-kvm shenzhen-kvm.c -lsodium -lX11
 * Wire protocol: ../PROTOCOL.md
 */
#include "src/00-base.c"
#include "src/10-config.c"
#include "src/15-desk.c"
#include "src/16-desk-x11.c"
#include "src/17-desk-weston.c"
#include "src/20-helper.c"
#include "src/30-main.c"
#include "src/35-run.c"
#include "src/40-agent.c"
#include "src/50-cli.c"
