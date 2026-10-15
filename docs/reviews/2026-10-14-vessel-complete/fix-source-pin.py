from pathlib import Path
p=Path('source/tests/test_goo_toon_math.cpp');s=p.read_text();start=s.index('        CHECK_MSG(primitive.find("glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),\\n');end=s.index(';',start)+1;s=s[:start]+'''        auto compact_primitive = primitive;
        std::erase_if(compact_primitive, [](char c) { return c == ' ' || c == '\\t' || c == '\\r' || c == '\\n'; });
        CHECK_MSG(compact_primitive.find("glm::vec4(1.0f,1.0f,1.0f,1.0f),glm::vec4(-1.0f,-1.0f,-1.0f,-1.0f)") != deren::utility::vessel::string::npos,
                  "both rim lanes' neutrals in `toon_inputs`");'''+s[end:];p.write_text(s)
