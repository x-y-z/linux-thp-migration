#ifndef MIGRATE_MODE_H_INCLUDED
#define MIGRATE_MODE_H_INCLUDED
/*
 * MIGRATE_ASYNC means never block
 * MIGRATE_SYNC_LIGHT in the current implementation means to allow blocking
 *	on most operations but not ->writepage as the potential stall time
 *	is too significant
 * MIGRATE_SYNC will block when migrating pages
 */
enum migrate_mode {
	MIGRATE_ASYNC		= 1<<0,
	MIGRATE_SYNC_LIGHT	= 1<<1,
	MIGRATE_SYNC		= 1<<2,
	MIGRATE_ST		= 1<<3,
	MIGRATE_MT		= 1<<4,
	MIGRATE_CONCUR		= 1<<5,
};

#endif		/* MIGRATE_MODE_H_INCLUDED */
