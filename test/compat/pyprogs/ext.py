import markupsafe
import markupsafe._speedups as speedups
print(speedups.__name__, markupsafe.escape("<a>"), speedups.__file__.endswith("-linux-musl.so"))
