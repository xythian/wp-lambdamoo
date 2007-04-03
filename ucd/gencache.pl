#!/usr/bin/perl

# Configurable parameters...
$associativity = 4;
$cache_rows = 256;

$maxent = $associativity-1;

open(GEN, '>', "gen/cache.c") or die;

print GEN "#define CACHE_ROWS $cache_rows\n";
print GEN "#define ASSOCIATIVITY $associativity\n";
print GEN "\n";

print GEN "struct cache_entry {\n";
print GEN "\tint32_t ucs;\n";
print GEN "\tconst struct unicode_character_data *ucd;\n";
print GEN "};\n\n";

print GEN "struct cache_row {\n";
print GEN "#ifdef HAVE_PTHREAD_H\n";
print GEN "\tpthread_mutex_t mutex;\n";
print GEN "#endif\n";
print GEN "\tstruct cache_entry e[$associativity];\n";
print GEN "};\n\n";

print GEN "static struct cache_row libucd_cache[$cache_rows] = {\n";

for ( $i = 0 ; $i < $cache_rows ; $i++ ) {
    print GEN "\t{\n";
    print GEN "#ifdef HAVE_PTHREAD_H\n";
    print GEN "\t\tPTHREAD_MUTEX_INITIALIZER,\n";
    print GEN "#endif\n";
    print GEN "\t\t{\n";
    for ( $j = 0 ; $j < $associativity ; $j++ ) {
	print GEN "\t\t\t{ -1, 0 },\n";
    }
    print GEN "\t\t},\n";
    print GEN "\t},\n";
}
print GEN "};\n\n";

print GEN "#define RETURN_ENTRY(u,r) \\\n";
print GEN "\tlock_cache(r); \\\n";

for ( $i = 0 ; $i < $associativity ; $i++ ) {
    print GEN "\tif ( u == r->e[$i].ucs ) { \\\n";
    print GEN "\t\tconst struct unicode_character_data *ucd = r->e[$i].ucd; \\\n";
    if ( $i > 0 ) {
	for ( $j = $i ; $j > 0 ; $j-- ) {
	    $jm1 = $j-1;
	    print GEN "\t\tr->e[$j] = r->e[$jm1]; \\\n";
	}
	print GEN "\t\tr->e[0].ucs = u; r->e[0].ucd = ucd; \\\n";
    }
    print GEN "\t\tucd = unicode_character_get(ucd); \\\n";
    print GEN "\t\tunlock_cache(r); \\\n";
    print GEN "\t\treturn ucd; \\\n";
    print GEN "\t} \\\n";
}

print GEN "\tunlock_cache(r); \\\n";
print GEN "\tif ( (ucd = _libucd_character_data_raw(u)) ) { \\\n";
print GEN "\t\tconst struct unicode_character_data *olducd; \\\n";
print GEN "\t\tlock_cache(r); \\\n";
print GEN "\t\tolducd = r->e[$maxent].ucd; \\\n";
for ( $j = $maxent ; $j > 0 ; $j-- ) {
    $jm1 = $j-1;
    print GEN "\t\tr->e[$j] = r->e[$jm1]; \\\n";
}
print GEN "\t\tr->e[0].ucs = u; r->e[0].ucd = ucd; \\\n";
print GEN "\t\tunlock_cache(r); \\\n";
print GEN "\t\tif (olducd) \\\n";
print GEN "\t\t\tunicode_character_put(olducd); \\\n";
print GEN "\t} \\\n";
print GEN "\treturn ucd; \\\n";
print GEN "\n";

close(GEN);
