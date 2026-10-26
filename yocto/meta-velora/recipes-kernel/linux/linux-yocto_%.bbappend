FILESEXTRAPATHS:prepend := "${THISDIR}/files:"
# TODO(Story 1.1): preempt-rt.cfg is a PLACEHOLDER for the validated PREEMPT_RT fragment.
# TODO(Story 1.2): audio config fragment (I2S/TDM, PCM3168A, UAC2 gadget) is not yet provided.
SRC_URI:append = " file://preempt-rt.cfg file://velora-dev.cfg file://velora-rauc.cfg"
