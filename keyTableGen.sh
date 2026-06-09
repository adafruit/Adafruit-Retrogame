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
# Wrap KEY_MACRO1 onward in #if 0 (KEY_MACRO itself stays enabled);
# insert the guard exactly once after the KEY_MACRO entry.
grep '#define* KEY_' $1 | awk '
  { print "\t{ \"" substr($2, 5) "\", " $2 " }," }
  /MACRO/ && !seen { print "#if 0 // Esoteric keys disabled, smaller program size; edit if needed"; seen = 1 }
'
echo "#endif"
echo "\t{ NULL, -1 } // END-OF-LIST"
echo "};"
