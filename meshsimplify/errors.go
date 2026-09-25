package meshsimplify

import "strconv"

// Error codes returned by the native library (see internal/libmesh/mesh.h).
const (
	libOK           = 0
	libErrMemory    = 1
	libErrInvalid   = 2
	libErrOpenFile  = 3
	libErrBadFormat = 4
	libErrCancelled = 5
	libErrNotReady  = 6
	libErrRatio     = 7
)

// Error wraps a non-zero status code returned by the native mesh library.
type Error struct {
	Code int
	Op   string
}

func (e *Error) Error() string {
	why := map[int]string{
		libErrMemory:    "out of memory in native library",
		libErrInvalid:   "invalid argument or handle",
		libErrOpenFile:  "cannot open file",
		libErrBadFormat: "malformed or unsupported model file",
		libErrCancelled: "simplification cancelled",
		libErrNotReady:  "mesh contains no geometry",
		libErrRatio:     "ratio must be in the interval (0,1]",
	}
	msg, ok := why[e.Code]
	if !ok {
		msg = "unknown native error " + strconv.Itoa(e.Code)
	}
	return "meshsimplify: " + e.Op + ": " + msg
}

func libErr(op string, code int32) error {
	if code == libOK {
		return nil
	}
	return &Error{Code: int(code), Op: op}
}
