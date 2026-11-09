/* the utility QoS class: below normal priority (ONNX Runtime's threads inherit the class) */
#ifndef RBSP_WIN_QOS_H
#define RBSP_WIN_QOS_H
#define QOS_CLASS_UTILITY 0x11
static int pthread_set_qos_class_self_np(int cls, int rel) { (void)cls; (void)rel; return SetPriorityClass(GetCurrentProcess(), BELOW_NORMAL_PRIORITY_CLASS) ? 0 : -1; }
#endif
