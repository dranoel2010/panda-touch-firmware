# Soft-White-Oberfläche: ui/ (Engine + Logik), ui/gen/ (aus dem Prototyp erzeugt), ui/fonts/ (Manrope)
file(GLOB UI_SRCS "ui/*.c" "ui/gen/*.c" "ui/fonts/*.c")
idf_component_register(SRCS "main.c" "sw_debug.c" "drucker.c" ${UI_SRCS}
                       INCLUDE_DIRS "." "ui")
