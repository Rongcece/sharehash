# 或者 ~/.gdbinit

# 启用漂亮打印
set print pretty on

# 跳过规则
skip -gfi */tsan/*
skip -gfi */libsanitizer/*
skip -gfi */sysdeps/*
skip -gfi */gmon/*
skip -gfi *_mcount.S
skip -gfi /usr/include/c++/*
skip -gfi /usr/include/*
skip -gfi */bits/*
skip -gfi *atomic*
skip -gfi */boost/*

echo GDB skip rules loaded\n