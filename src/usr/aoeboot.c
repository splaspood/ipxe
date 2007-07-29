#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <gpxe/aoe.h>
#include <gpxe/ata.h>
#include <gpxe/netdevice.h>
#include <gpxe/dhcp.h>
#include <int13.h>
#include <usr/aoeboot.h>

/**
 * Guess boot network device
 *
 * @ret netdev		Boot network device
 */
static struct net_device * guess_boot_netdev ( void ) {
	struct net_device *boot_netdev;

	/* Just use the first network device */
	for_each_netdev ( boot_netdev ) {
		return boot_netdev;
	}

	return NULL;
}

int aoeboot ( const char *root_path ) {
	struct ata_device ata;
	struct int13_drive drive;
	int rc;

	memset ( &ata, 0, sizeof ( ata ) );
	memset ( &drive, 0, sizeof ( drive ) );

	printf ( "AoE booting from %s\n", root_path );

	/* FIXME: ugly, ugly hack */
	struct net_device *netdev = guess_boot_netdev();

	if ( ( rc = aoe_attach ( &ata, netdev, root_path ) ) != 0 ) {
		printf ( "Could not attach AoE device: %s\n",
			 strerror ( rc ) );
		goto error_attach;
	}
	if ( ( rc = init_atadev ( &ata ) ) != 0 ) {
		printf ( "Could not initialise AoE device: %s\n",
			 strerror ( rc ) );
		goto error_init;
	}

	drive.drive = find_global_dhcp_num_option ( DHCP_EB_BIOS_DRIVE );
	drive.blockdev = &ata.blockdev;

	register_int13_drive ( &drive );
	printf ( "Registered as BIOS drive %#02x\n", drive.drive );
	printf ( "Booting from BIOS drive %#02x\n", drive.drive );
	rc = int13_boot ( drive.drive );
	printf ( "Boot failed\n" );

	printf ( "Unregistering BIOS drive %#02x\n", drive.drive );
	unregister_int13_drive ( &drive );

 error_init:
	aoe_detach ( &ata );
 error_attach:
	return rc;
}
