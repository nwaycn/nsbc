#include "location_db.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    loc_db_cfg_t cfg = {
        .capacity = 1,
        .max_contacts = 1
    };
    loc_db_t *db = NULL;
    assert(loc_db_create(&cfg, &db) == NSBC_OK);

    loc_binding_t first = { 0 };
    snprintf(first.aor, sizeof(first.aor), "%s", "alice@example.com");
    snprintf(first.contact, sizeof(first.contact), "%s", "sip:first@example.net");
    snprintf(first.callid, sizeof(first.callid), "%s", "first-call");
    first.expire_ms = 10000;
    first.cseq = 1;
    assert(loc_upsert(db, &first, 1) == LOC_OK);
    assert(loc_db_count(db) == 1);

    loc_binding_t replacement = { 0 };
    snprintf(replacement.aor, sizeof(replacement.aor), "%s", first.aor);
    snprintf(replacement.contact, sizeof(replacement.contact), "%s",
             "sip:replacement@example.net");
    snprintf(replacement.callid, sizeof(replacement.callid), "%s",
             "replacement-call");
    replacement.expire_ms = 20000;
    replacement.cseq = 1;
    assert(loc_upsert(db, &replacement, 2) == LOC_OK);
    assert(loc_db_count(db) == 1);

    loc_binding_t result = { 0 };
    assert(loc_query(db, first.aor, &result, 1, 2) == 1);
    assert(strcmp(result.contact, replacement.contact) == 0);
    assert(loc_remove(db, first.aor, first.contact) == 0);
    assert(loc_remove(db, first.aor, replacement.contact) == 1);
    assert(loc_db_count(db) == 0);

    loc_db_destroy(db);
    puts("location db: PASS");
    return 0;
}
