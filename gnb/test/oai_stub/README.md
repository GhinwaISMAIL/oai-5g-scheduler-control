Minimal stand-ins for the gNB headers, used only to compile and exercise the
controllable downlink policy outside the gNB build.  The struct and macro
definitions are transcribed from the pinned source (see ../../etc/stack-versions.json);
`stubs.c` implements the few helper functions the policy calls.  This checks the
policy's logic and its use of those types.  It does not replace compiling inside
the gNB image.
