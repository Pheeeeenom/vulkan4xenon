"""changed.py <shaders.c> <broken shaders.c> <label>: the shaders whose containers a break set changed, so a set is seen
to reach only the shaders its checks draw with."""
import re
import sys


def containers(path):
    text = open(path, encoding="utf-8").read()
    return dict(re.findall(r"static const uint32_t (\w+)_container\[\] = \{([^}]*)\};", text))


before, after = containers(sys.argv[1]), containers(sys.argv[2])
changed = sorted(name for name in before if before[name] != after.get(name))
print(f"{sys.argv[3]}: {len(changed)} shaders changed: {', '.join(changed) or 'none'}")
