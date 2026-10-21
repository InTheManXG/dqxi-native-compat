"""在宿主执行同一 CoreFoundation 时区实现，不依赖 /etc/localtime。"""
from pathlib import Path
import subprocess
import tempfile

source = Path(__file__).resolve().parents[1].joinpath('module_probe.c').read_text()
function = source.split('static int local_zone(char name[36]) {', 1)[1].split('\n}\n', 1)[0]
with tempfile.TemporaryDirectory() as directory:
    root = Path(directory)
    test = root / 'timezone.c'
    test.write_text('''#include <CoreFoundation/CoreFoundation.h>
#include <assert.h>
#include <string.h>
#undef TARGET_OS_IPHONE
#define TARGET_OS_IPHONE 1
static int local_zone(char name[36]) {''' + function + '''
}
int main(void) {
    const char *names[] = {"Asia/Shanghai", "America/Argentina/Buenos_Aires"};
    for (unsigned i = 0; i < 2; i++) {
        CFStringRef name = CFStringCreateWithCString(NULL, names[i], kCFStringEncodingUTF8);
        CFTimeZoneRef zone = CFTimeZoneCreateWithName(NULL, name, false);
        assert(zone); CFTimeZoneSetDefault(zone);
        char result[36]; memset(result, 0xff, sizeof result);
        assert(local_zone(result)); assert(!strcmp(result, names[i])); assert(result[35] == 0);
        CFRelease(zone); CFRelease(name);
    }
}
''')
    subprocess.run(['xcrun', 'clang', str(test), '-framework', 'CoreFoundation', '-o', str(root/'test')], check=True)
    subprocess.run([str(root/'test')], check=True)
print('system timezone: IANA names and fixed-size output OK')
