---
title: errno
---

# `errno` — POSIX error constants

```python
import errno
```

Numeric constants for the errno values that `OSError` exceptions
carry. Useful for distinguishing kinds of failures:

```python
import errno
try:
    open('NOPE.TXT')
except OSError as e:
    if e.args[0] == errno.ENOENT:
        print('file not found')
    elif e.args[0] == errno.EACCES:
        print('permission denied')
    else:
        raise
```

## Constants

The full list available in this port (matches the Linux subset
uc386's libc ships):

`EPERM EACCES EAGAIN EBADF EBUSY ECHILD EDOM EEXIST EFAULT EFBIG`
`EINTR EINVAL EIO EISDIR EMFILE EMLINK ENAMETOOLONG ENFILE ENODEV`
`ENOENT ENOEXEC ENOMEM ENOSPC ENOSYS ENOTBLK ENOTDIR ENOTEMPTY`
`ENOTTY ENXIO EOF EPIPE ERANGE EROFS ESPIPE ESRCH ETIMEDOUT ETXTBSY`
`EXDEV`

⚠️ The DOS errno set is smaller — most BSD/POSIX-flavoured
errnos (`EOPNOTSUPP`, `EADDRINUSE`, etc.) aren't present because
the underlying DOS calls never produce them. The lwIP socket layer
maps lwIP errors to POSIX errnos itself.

### `errno.errorcode` — `dict[int, str]`

Reverse lookup from errno number to name:

```python
>>> errno.errorcode[2]
'ENOENT'
>>> errno.errorcode[errno.EACCES]
'EACCES'
```

---

*Credit:
[MicroPython errno docs](https://docs.micropython.org/en/latest/library/errno.html)
(MIT).*
