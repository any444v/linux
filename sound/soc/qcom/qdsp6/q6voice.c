// SPDX-License-Identifier: GPL-2.0
// Copyright (c) 2012-2017, The Linux Foundation. All rights reserved.
// Copyright (c) 2020, Stephan Gerhold

#include <linux/device.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include "q6afe.h"
#include "q6cvp.h"
#include "q6cvs.h"
#include "q6mvm.h"
#include "q6voice-common.h"

#define VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS		0x00010F71
#define VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT		0x00010F77

struct q6voice_path_runtime {
	struct q6voice_session *sessions[Q6VOICE_SERVICE_COUNT];
	unsigned int started;
	/* AFE ports the vocproc session was created for */
	int tx_port, rx_port;
};

struct q6voice_path {
	struct q6voice *v;

	enum q6voice_path_type type;
	int tx_port, rx_port;
	u32 tx_topo, rx_topo;
	/* Uplink (microphone) muted */
	bool tx_mute;
	/* Serialize access to voice path session */
	struct mutex lock;
	struct q6voice_path_runtime *runtime;
};

struct q6voice {
	struct device *dev;
	bool cvd_v2_3;
	struct q6voice_path paths[Q6VOICE_PATH_COUNT];
};

static inline struct q6voice_session *session_create(enum q6voice_path_type type,
						     int tx_port, int rx_port,
						     u32 tx_topo, u32 rx_topo)
{
	return q6cvp_session_create(type, tx_port, rx_port, tx_topo, rx_topo);
}

static struct q6voice_session *session_create_v3(enum q6voice_path_type type,
						 int tx_port, int rx_port,
						 u32 tx_topo, u32 rx_topo)
{
	struct q6voice_session *cvp;

	cvp = q6cvp_session_create_v3(type, tx_port, rx_port, tx_topo, rx_topo);
	if (cvp == NULL)
		return NULL;

	q6cvp_send_channel_info(cvp, false);
	q6cvp_send_channel_info(cvp, true);

	q6cvp_send_media_format(cvp, rx_port, false);
	q6cvp_send_media_format(cvp, tx_port, true);

	q6cvp_topology_commit(cvp);

	return cvp;
}

static int q6voice_path_start(struct q6voice_path *p)
{
	struct device *dev = p->v->dev;
	struct q6voice_session *mvm, *cvp;
	int ret;

	dev_dbg(dev, "start path %d\n", p->type);

	mvm = p->runtime->sessions[Q6VOICE_SERVICE_MVM];
	if (!mvm) {
		mvm = q6mvm_session_create(p->type);
		if (IS_ERR(mvm))
			return PTR_ERR(mvm);
		p->runtime->sessions[Q6VOICE_SERVICE_MVM] = mvm;
	}

	cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	if (!cvp) {
		if (p->v->cvd_v2_3)
			cvp = session_create_v3(p->type,
						q6afe_get_port_id(p->tx_port),
						q6afe_get_port_id(p->rx_port),
						p->tx_topo, p->rx_topo);
		else
			cvp = session_create(p->type,
					     q6afe_get_port_id(p->tx_port),
					     q6afe_get_port_id(p->rx_port),
					     p->tx_topo, p->rx_topo);

		if (IS_ERR(cvp))
			return PTR_ERR(cvp);
		p->runtime->sessions[Q6VOICE_SERVICE_CVP] = cvp;
		p->runtime->tx_port = p->tx_port;
		p->runtime->rx_port = p->rx_port;
	}

	ret = q6cvp_enable(cvp, true);
	if (ret) {
		dev_err(dev, "failed to enable cvp: %d\n", ret);
		goto cvp_err;
	}

	ret = q6mvm_attach(mvm, cvp, true);
	if (ret) {
		dev_err(dev, "failed to attach cvp to mvm: %d\n", ret);
		goto attach_err;
	}

	ret = q6mvm_start(mvm, true);
	if (ret) {
		dev_err(dev, "failed to start voice: %d\n", ret);
		goto start_err;
	}

	if (p->tx_mute && q6cvp_set_mute(cvp, true, true))
		dev_warn(dev, "failed to mute the uplink of path %d\n", p->type);

	return ret;

start_err:
	q6mvm_start(mvm, false);
attach_err:
	q6mvm_attach(mvm, cvp, false);
cvp_err:
	q6cvp_enable(cvp, false);
	return ret;
}

int q6voice_start(struct q6voice *v, enum q6voice_path_type path, bool capture)
{
	struct q6voice_path *p = &v->paths[path];
	int ret = 0;

	mutex_lock(&p->lock);
	if (!p->runtime) {
		p->runtime = kzalloc(sizeof(*p->runtime), GFP_KERNEL);
		if (!p->runtime) {
			ret = -ENOMEM;
			goto out;
		}
	}

	if (p->runtime->started & BIT(capture)) {
		ret = -EALREADY;
		goto out;
	}

	p->runtime->started |= BIT(capture);

	/* FIXME: For now we only start if both RX/TX are active */
	if (p->runtime->started != 3)
		goto out;

	/*
	 * The PCM may be opened (e.g. by q6voiced when the call becomes
	 * active) before the mixer controls have selected the AFE ports.
	 * Start the path once both are set, see q6voice_path_reroute().
	 */
	if (!p->tx_port || !p->rx_port) {
		dev_dbg(v->dev, "path %d waits for its ports\n", path);
		goto out;
	}

	ret = q6voice_path_start(p);
	if (ret) {
		p->runtime->started &= ~BIT(capture);
		dev_err(v->dev, "failed to start path %d: %d\n", path, ret);
		goto out;
	}

out:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_start);

static void q6voice_path_stop(struct q6voice_path *p)
{
	struct device *dev = p->v->dev;
	struct q6voice_session *mvm = p->runtime->sessions[Q6VOICE_SERVICE_MVM];
	struct q6voice_session *cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
	int ret;

	dev_dbg(dev, "stop path %d\n", p->type);

	if (!mvm || !cvp)
		return;

	ret = q6mvm_start(mvm, false);
	if (ret)
		dev_err(dev, "failed to stop voice: %d\n", ret);

	ret = q6mvm_attach(mvm, cvp, false);
	if (ret)
		dev_err(dev, "failed to detach cvp from mvm: %d\n", ret);

	ret = q6cvp_enable(cvp, false);
	if (ret)
		dev_err(dev, "failed to disable cvp: %d\n", ret);
}

static void q6voice_path_destroy(struct q6voice_path *p)
{
	struct q6voice_path_runtime *runtime = p->runtime;
	enum q6voice_service_type svc;

	for (svc = 0; svc < Q6VOICE_SERVICE_COUNT; ++svc) {
		if (runtime->sessions[svc])
			q6voice_session_release(runtime->sessions[svc]);
	}

	p->runtime = NULL;
	kfree(runtime);
}

/*
 * The vocproc (CVP) session is bound to the AFE ports it was created with.
 * When a running call moves to another device (earpiece -> speaker), stop
 * the path, drop the CVP session and start again with the new ports.
 */
static void q6voice_path_reroute(struct q6voice_path *p)
{
	struct q6voice_path_runtime *rt = p->runtime;
	struct q6voice_session *cvp;
	int ret;

	if (!rt || rt->started != 3)
		return;
	/* 0 is the transient state while one device is off and the next not yet on */
	if (!p->tx_port || !p->rx_port)
		return;
	if (rt->tx_port == p->tx_port && rt->rx_port == p->rx_port)
		return;

	dev_dbg(p->v->dev, "reroute path %d: rx %d -> %d, tx %d -> %d\n",
		p->type, rt->rx_port, p->rx_port, rt->tx_port, p->tx_port);

	q6voice_path_stop(p);

	cvp = rt->sessions[Q6VOICE_SERVICE_CVP];
	if (cvp) {
		q6voice_session_release(cvp);
		rt->sessions[Q6VOICE_SERVICE_CVP] = NULL;
	}

	ret = q6voice_path_start(p);
	if (ret)
		dev_err(p->v->dev, "failed to reroute path %d: %d\n", p->type, ret);
}

int q6voice_stop(struct q6voice *v, enum q6voice_path_type path, bool capture)
{
	struct q6voice_path *p = &v->paths[path];
	int ret = 0;

	mutex_lock(&p->lock);
	if (!p->runtime || !(p->runtime->started & BIT(capture)))
		goto out;

	if (p->runtime->started == 3)
		q6voice_path_stop(p);

	p->runtime->started &= ~BIT(capture);

	if (p->runtime->started == 0)
		q6voice_path_destroy(p);

out:
	mutex_unlock(&p->lock);
	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_stop);

static void q6voice_free(void *data)
{
	struct q6voice *v = data;
	enum q6voice_path_type path;

	for (path = 0; path < Q6VOICE_PATH_COUNT; ++path) {
		struct q6voice_path *p = &v->paths[path];

		mutex_lock(&p->lock);
		if (p->runtime) {
			dev_warn(v->dev,
				 "q6voice_remove() called while path %d is active\n",
				 path);

			if (p->runtime->started == 3)
				q6voice_path_stop(p);
			q6voice_path_destroy(p);
		}
		mutex_unlock(&p->lock);
		mutex_destroy(&p->lock);
	}
}

struct q6voice *q6voice_create(struct device *dev, bool cvd_v2_3)
{
	struct q6voice *v;
	enum q6voice_path_type path;
	int ret;

	v = devm_kzalloc(dev, sizeof(*v), GFP_KERNEL);
	if (!v)
		return ERR_PTR(-ENOMEM);

	v->dev = dev;
	v->cvd_v2_3 = cvd_v2_3;

	for (path = 0; path < Q6VOICE_PATH_COUNT; ++path) {
		struct q6voice_path *p = &v->paths[path];

		p->v = v;
		p->type = path;
		p->tx_topo = VSS_IVOCPROC_TOPOLOGY_ID_TX_SM_ECNS;
		p->rx_topo = VSS_IVOCPROC_TOPOLOGY_ID_RX_DEFAULT;
		mutex_init(&p->lock);
	}

	ret = devm_add_action(dev, q6voice_free, v);
	if (ret)
		return ERR_PTR(ret);

	return v;
}
EXPORT_SYMBOL_GPL(q6voice_create);

int q6voice_get_port(struct q6voice *v, enum q6voice_path_type path,
		     bool capture)
{
	struct q6voice_path *p = &v->paths[path];

	if (capture)
		return p->tx_port;
	else
		return p->rx_port;
}
EXPORT_SYMBOL_GPL(q6voice_get_port);

void q6voice_set_port(struct q6voice *v, enum q6voice_path_type path,
		      bool capture, int index)
{
	struct q6voice_path *p = &v->paths[path];

	mutex_lock(&p->lock);
	if (capture)
		p->tx_port = index;
	else
		p->rx_port = index;

	q6voice_path_reroute(p);
	mutex_unlock(&p->lock);
}
EXPORT_SYMBOL_GPL(q6voice_set_port);

u32 q6voice_get_topology(struct q6voice *v, enum q6voice_path_type path,
			 bool capture)
{
	struct q6voice_path *p = &v->paths[path];

	if (capture)
		return p->tx_topo;
	else
		return p->rx_topo;
}
EXPORT_SYMBOL_GPL(q6voice_get_topology);

void q6voice_set_topology(struct q6voice *v, enum q6voice_path_type path,
			  bool capture, u32 topo)
{
	struct q6voice_path *p = &v->paths[path];

	if (capture)
		p->tx_topo = topo;
	else
		p->rx_topo = topo;
}
EXPORT_SYMBOL_GPL(q6voice_set_topology);

bool q6voice_get_mute(struct q6voice *v, enum q6voice_path_type path)
{
	return v->paths[path].tx_mute;
}
EXPORT_SYMBOL_GPL(q6voice_get_mute);

/* Mute the uplink (microphone) of a voice path, now and when it starts */
int q6voice_set_mute(struct q6voice *v, enum q6voice_path_type path, bool mute)
{
	struct q6voice_path *p = &v->paths[path];
	struct q6voice_session *cvp;
	int ret = 0;

	mutex_lock(&p->lock);
	p->tx_mute = mute;
	if (p->runtime && p->runtime->started == 3) {
		cvp = p->runtime->sessions[Q6VOICE_SERVICE_CVP];
		if (cvp)
			ret = q6cvp_set_mute(cvp, true, mute);
	}
	mutex_unlock(&p->lock);

	return ret;
}
EXPORT_SYMBOL_GPL(q6voice_set_mute);

MODULE_AUTHOR("Stephan Gerhold <stephan@gerhold.net>");
MODULE_DESCRIPTION("Q6Voice driver");
MODULE_LICENSE("GPL v2");
