Import("env")

import os

if env.IsIntegrationDump():
    Return()

project_dir = env.subst("$PROJECT_DIR")
build_dir = env.subst("$BUILD_DIR")
data_dir = os.path.join(project_dir, "data")
header_path = os.path.join(build_dir, "ledal_embedded_index.h")

assets = [
    ("INDEX_HTML", os.path.join(data_dir, "index.html"), "LDLHTML"),
    ("EXAMPLES_JSON", os.path.join(data_dir, "examples.json"), "LDLJSON"),
]

os.makedirs(build_dir, exist_ok=True)
header_parts = [
    "#pragma once\n",
    "#include <Arduino.h>\n",
    "namespace ledlab { namespace web {\n",
]

for symbol, source_path, delimiter in assets:
    with open(source_path, "r", encoding="utf-8", newline="") as source_file:
        content = source_file.read()

    if ")" + delimiter + '"' in content:
        raise RuntimeError(
            "The " + os.path.basename(source_path) +
            " file contains the embedded C++ raw-string delimiter"
        )

    header_parts.extend([
        "static const char " + symbol + "[] PROGMEM = R\"" + delimiter + "(\n",
        content,
        "\n)" + delimiter + "\";\n",
    ])

header_parts.append("} }  // namespace ledlab::web\n")

with open(header_path, "w", encoding="utf-8", newline="") as header_file:
    header_file.write("".join(header_parts))

env.Append(CPPPATH=[build_dir])
print("Embedded LED-AL web assets generated: " + header_path)
