"""将已验证的透传几何程序的选层操作并入顶点输出。"""
import re


def layered_vertex(source):
    # 只接受当前翻译器的单出口结构，拒绝误改输入结构或遗漏其他返回分支。
    output = re.search(r"struct main0_out\s*\{(.*?)\n\};", source, re.S)
    field = re.search(r"float4\s+(\w+)\s*\[\[user\(locn1\)\]\]", output[1]) if output else None
    if not field or source.count("return out;") != 1:
        raise ValueError("unsupported passthrough vertex output")
    source = source[:output.end(1)] + "\n    uint dm_layer [[render_target_array_index]];" + source[output.end(1):]
    # 原几何程序只发射三角形、不传递 PointSize；Metal 三角形管线禁止顶点输出点大小。
    source = source.replace("    float gl_PointSize [[point_size]];\n", "")
    source = source.replace("    out.gl_PointSize = 1.0;\n", "")
    return source.replace("return out;", f"out.dm_layer = as_type<uint>(out.{field[1]}.x);\n    return out;")
