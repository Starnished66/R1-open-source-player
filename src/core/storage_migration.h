#ifndef STORAGE_MIGRATION_H
#define STORAGE_MIGRATION_H

/* Temporary compatibility migration for installations upgrading from the
 * pre-.compas layout. Call after a card is mounted and again after hot-swap. */
void storage_migrate_legacy_data_at(int rootfd);
void storage_migrate_internal_data(void);
void storage_migrate_legacy_data(void);

#endif
