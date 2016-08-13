echo "// This file is software-generated; do not edit. See $0 instead."
echo
echo "#include <$1>"
echo
echo "typedef struct {"
echo "\tchar *name;"
echo "\tint   value;"
echo "} dict;"
echo
echo "dict keyTable[] = {"
grep '#define* KEY_' $1 | awk '{ print "\t{ \"" substr($2,5) "\", " $2 " }," }'
echo "\t{ NULL, -1 } // END-OF-LIST"
echo "};"
