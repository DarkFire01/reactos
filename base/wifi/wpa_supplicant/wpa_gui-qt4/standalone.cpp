#include "standalone.h"

#include <QDebug>
#include <QByteArray>
#include <QApplication>
#include <QPointer>

#define WPA_DEBUG_H
#include "includes.h"
#undef WPA_DEBUG_H
#include "common/wpa_ctrl.h"
extern "C" {
#include "ctrl_iface.h"
#include "eloop.h"
#include "wpa_supplicant_i.h"
#include "utils/wpa_debug.h"
// main_winsvc.c
int wpa_supplicant_thread(void);
int wpa_supplicant_run_call(struct wpa_global *global);
}

namespace {

class ScopedLogger
{
public:
	ScopedLogger(const char *scope, const QByteArray &arg = QByteArray())
		: m_scope(scope)
		, m_arg(arg)
	{
		if (const char *c = strrchr(scope, ':'))
			m_scope = c + 1;
		if (m_arg.isEmpty())
			qDebug() << "=>" << m_scope;
		else
			qDebug() << "=>" << m_scope << m_arg;
	}

	~ScopedLogger()
	{
		if (m_arg.isEmpty())
			qDebug() << "<=" << m_scope;
		else
			qDebug() << "<=" << m_scope << m_arg;
	}

	void trace(const QByteArray &desc) const
	{
		if (m_arg.isEmpty())
			qDebug() << "<...>" << m_scope << ":" << desc;
		else
			qDebug() << "<...>" << m_scope << m_arg << ":" << desc;
	}

private:
	const char *m_scope;
	const QByteArray m_arg;
};

}

#ifndef NDEBUG
#define SCOPE_LOG(S) const ScopedLogger scoped_logger((S))
#define SCOPE_LOG_ARG(S, A) const ScopedLogger scoped_logger((S), (A))
#define SCOPE_LOG_TRACE(D) scoped_logger.trace((D))
#else
#define SCOPE_LOG(S)
#define SCOPE_LOG_ARG(S, A)
#define SCOPE_LOG_TRACE(D)
#endif

struct ctrl_iface_priv
{
	wpa_supplicant *wpa_s;
};

struct ctrl_iface_global_priv
{
	wpa_global *global;
};

struct wpa_ctrl
{
	QByteArray ctrl_path;
	bool attached;
	int debug_level;
	QList<QByteArray> messages;

	wpa_ctrl()
		: attached(false)
		, debug_level(MSG_INFO)
	{}
};

extern "C" {

static void wpa_supplicant_ctrl_iface_msg_cb(void *ctx, int level, wpa_msg_type type, const char *txt, size_t len)
{
	wpa_supplicant *wpa_s = (wpa_supplicant *)ctx;
	if (wpa_s == NULL || wpa_s->ctrl_iface == NULL)
		return;

	StandaloneSupplicant *s = StandaloneSupplicant::instance();
	if (!s->isRunning())
		return;

	s->wpa_supplicant_ctrl_iface_send(wpa_s->ctrl_iface, level, txt, len);
}

}

StandaloneSupplicant::StandaloneSupplicant(QObject *parent)
	: QThread(parent)
	, m_global(NULL)
	, m_supplicant_running(false)
{}

StandaloneSupplicant::~StandaloneSupplicant()
{
	if (isRunning())
		stop();
}

wpa_supplicant *StandaloneSupplicant::get_supplicant(const wpa_ctrl *ctrl)
{
	wpa_supplicant *wpa_s = NULL;
	ctrl_iface_priv *iface = NULL;
	m_ctrl_ifaces_lock.lockForRead();
	for (QList<ctrl_iface_priv*>::Iterator it = m_ctrl_ifaces.begin(); it != m_ctrl_ifaces.end(); ++it) {
		if (ctrl->ctrl_path == (*it)->wpa_s->ifname) {
			iface = *it;
			break;
		}
	}
	if (iface)
		wpa_s = iface->wpa_s;
	m_ctrl_ifaces_lock.unlock();
	return wpa_s;
}

StandaloneSupplicant *StandaloneSupplicant::instance()
{
	static QPointer<StandaloneSupplicant> s;
	if (!s) {
		s = new StandaloneSupplicant(qApp);
		if (s->thread() != qApp->thread())
			s->moveToThread(qApp->thread());
		s->setObjectName("StandaloneSupplicant");
	}
	return s;
}

void StandaloneSupplicant::wpa_supplicant_ctrl_iface_send(ctrl_iface_priv *priv, int level, const char *buf, size_t len)
{
	SCOPE_LOG_ARG(__FUNCTION__, QByteArray(buf, len));
	m_wpa_ctrls_lock.lockForWrite();
	QByteArray message = QString::fromLatin1("<%1>").arg(level).toLatin1() + QByteArray(buf, len);
	for(QList<wpa_ctrl*>::Iterator it = m_wpa_ctrls.begin(); it != m_wpa_ctrls.end(); ++it) {
		if ((*it)->ctrl_path != priv->wpa_s->ifname)
			continue;
		if (level < (*it)->debug_level)
			continue;
		(*it)->messages.append(message);
	}
	m_wpa_ctrls_lock.unlock();
}

int StandaloneSupplicant::wpa_supplicant_run_call(wpa_global *global)
{
	m_supplicant_running = true;
	int result = wpa_supplicant_run(global);
	m_supplicant_running = false;
	return result;
}

void StandaloneSupplicant::wpa_supplicant_ctrl_iface_init(ctrl_iface_priv *priv)
{
	SCOPE_LOG(__FUNCTION__);
	m_ctrl_ifaces_lock.lockForWrite();
	m_ctrl_ifaces.append(priv);
	m_ctrl_ifaces_lock.unlock();
	wpa_msg_register_cb(&wpa_supplicant_ctrl_iface_msg_cb);
}

void StandaloneSupplicant::wpa_supplicant_ctrl_iface_deinit(ctrl_iface_priv *priv)
{
	SCOPE_LOG(__FUNCTION__);
	m_ctrl_ifaces_lock.lockForWrite();
	m_ctrl_ifaces.removeOne(priv);
	m_ctrl_ifaces_lock.unlock();
}

void StandaloneSupplicant::wpa_supplicant_ctrl_iface_wait(ctrl_iface_priv *priv)
{
	SCOPE_LOG(__FUNCTION__);
}

void StandaloneSupplicant::wpa_supplicant_global_ctrl_iface_init(ctrl_iface_global_priv *priv)
{
	SCOPE_LOG(__FUNCTION__);
	m_global_lock.lockForWrite();
	Q_ASSERT(!m_global);
	m_global = priv;
	m_global_lock.unlock();
}

void StandaloneSupplicant::wpa_supplicant_global_ctrl_iface_deinit(ctrl_iface_global_priv *priv)
{
	SCOPE_LOG(__FUNCTION__);
	m_global_lock.lockForWrite();
	Q_ASSERT(m_global == priv);
	m_global = NULL;
	m_global_lock.unlock();
}

void StandaloneSupplicant::wpa_ctrl_open(wpa_ctrl *ctrl)
{
	SCOPE_LOG_ARG(__FUNCTION__, QByteArray(ctrl->ctrl_path));
	m_wpa_ctrls_lock.lockForWrite();
	m_wpa_ctrls.append(ctrl);
	m_wpa_ctrls_lock.unlock();
}

void StandaloneSupplicant::wpa_ctrl_close(wpa_ctrl *ctrl)
{
	SCOPE_LOG_ARG(__FUNCTION__, QByteArray(ctrl->ctrl_path));
	m_wpa_ctrls_lock.lockForWrite();
	m_wpa_ctrls.removeOne(ctrl);
	m_wpa_ctrls_lock.unlock();
}

int StandaloneSupplicant::wpa_ctrl_request(wpa_ctrl *ctrl, const char *cmd, size_t cmd_len, char *reply, size_t *reply_len)
{
	SCOPE_LOG_ARG(__FUNCTION__, QByteArray(ctrl->ctrl_path));

	QByteArray buf(cmd, cmd_len);
	if (buf.length() >= CTRL_IFACE_MAX_LEN)
		buf.resize(CTRL_IFACE_MAX_LEN - 1);
	buf.append('\0');
	qDebug() << ">" << buf.constData();

	while(true) {
		int result = 0;
		char *r = NULL;
		size_t rlen = 0;
		if (ctrl->ctrl_path.isEmpty()) {
			m_global_lock.lockForRead();
			if ((!m_global || !m_supplicant_running) && isRunning()) {
				m_global_lock.unlock();
				SCOPE_LOG_TRACE("waiting for global");
				qApp->processEvents();
				msleep(100);
				continue;
			}
			wpa_global *global = (m_global ? m_global->global : NULL);
			m_global_lock.unlock();
			if (!global) {
				result = -1;
				rlen = 1;
			} else {
				r = wpa_supplicant_global_ctrl_iface_process(global, buf.data(), &rlen);
			}
		} else {
			wpa_supplicant *wpa_s = get_supplicant(ctrl);
			if (!wpa_s) {
				result = -1;
				rlen = 1;
			} else {
				r = wpa_supplicant_ctrl_iface_process(wpa_s, buf.data(), &rlen);
			}
		}
		if (r) {
			memset(reply, 0, *reply_len);
			if (rlen == 0) {
				const char *n = (const char*)memchr(r, '\n', qMin<size_t>(16, (*reply_len) - 1));
				if (n) {
					*reply_len = n - r + 1;
					memcpy(reply, r, *reply_len);
					reply[*reply_len] = '\0';
					*reply_len = 0;
				} else {
					*reply_len = 0;
					reply[0] = '\0';
				}
			} else {
				*reply_len = qMin(rlen, *reply_len);
				memcpy(reply, r, *reply_len);
				reply[*reply_len] = '\0';
			}
			free(r);
		} else {
			switch (rlen) {
			case 1:
				*reply_len = 5;
				memcpy(reply, "FAIL\n", *reply_len + 1);
				break;
			case 2:
				*reply_len = 3;
				memcpy(reply, "OK\n", *reply_len + 1);
				break;
			default:
				*reply = '\0';
				*reply_len = 0;
				break;
			}
		}
		qDebug() << "<" << reply;
		return result;
	}
}

int StandaloneSupplicant::wpa_ctrl_attach(wpa_ctrl *ctrl)
{
	SCOPE_LOG_ARG(__FUNCTION__, QByteArray(ctrl->ctrl_path));
	m_wpa_ctrls_lock.lockForWrite();
	ctrl->attached = true;
	m_wpa_ctrls_lock.unlock();
	wpa_supplicant *wpa_s = get_supplicant(ctrl);
	if (wpa_s)
		eapol_sm_notify_ctrl_attached(wpa_s->eapol);
	return 0;
}

int StandaloneSupplicant::wpa_ctrl_detach(wpa_ctrl *ctrl)
{
	SCOPE_LOG_ARG(__FUNCTION__, QByteArray(ctrl->ctrl_path));
	m_wpa_ctrls_lock.lockForWrite();
	ctrl->attached = false;
	ctrl->messages.clear();
	m_wpa_ctrls_lock.unlock();
	return 0;
}

int StandaloneSupplicant::wpa_ctrl_recv(wpa_ctrl *ctrl, char *reply, size_t *reply_len)
{
	SCOPE_LOG_ARG(__FUNCTION__, QByteArray(ctrl->ctrl_path));
	int result = 0;
	m_wpa_ctrls_lock.lockForWrite();
	if (ctrl->messages.empty()) {
		*reply_len = 0;
		result = -1;
	} else {
		const QByteArray &message = ctrl->messages.first();
		*reply_len = qMin<size_t>(*reply_len, message.size());
		memcpy(reply, message.constData(), *reply_len);
		ctrl->messages.removeFirst();
	}
	m_wpa_ctrls_lock.unlock();
	return result;
}

int StandaloneSupplicant::wpa_ctrl_pending(wpa_ctrl *ctrl)
{
	SCOPE_LOG_ARG(__FUNCTION__, QByteArray(ctrl->ctrl_path));
	m_wpa_ctrls_lock.lockForRead();
	int result = ctrl->messages.size();
	m_wpa_ctrls_lock.unlock();
	return result;
}

void StandaloneSupplicant::stop()
{
	SCOPE_LOG(__FUNCTION__);
	eloop_terminate();
	wait(4000);
}

void StandaloneSupplicant::run()
{
	SCOPE_LOG("SUPPLICANT THREAD");
	reset();
	wpa_supplicant_thread();
}

void StandaloneSupplicant::reset()
{
	SCOPE_LOG(__FUNCTION__);
	m_global_lock.lockForWrite();
	m_ctrl_ifaces_lock.lockForWrite();
	m_global = NULL;
	m_ctrl_ifaces.clear();
	m_global_lock.unlock();
	m_ctrl_ifaces_lock.unlock();
}

extern "C" {

struct ctrl_iface_priv *
wpa_supplicant_ctrl_iface_init(struct wpa_supplicant *wpa_s)
{
	ctrl_iface_priv *priv = new ctrl_iface_priv;
	priv->wpa_s = wpa_s;
	StandaloneSupplicant::instance()->wpa_supplicant_ctrl_iface_init(priv);
	return priv;
}

void
wpa_supplicant_ctrl_iface_deinit(struct wpa_supplicant *wpa_s,
				 struct ctrl_iface_priv *priv)
{
	StandaloneSupplicant::instance()->wpa_supplicant_ctrl_iface_deinit(priv);
	delete priv;
}

void
wpa_supplicant_ctrl_iface_wait(struct ctrl_iface_priv *priv)
{
	StandaloneSupplicant::instance()->wpa_supplicant_ctrl_iface_wait(priv);
}

struct ctrl_iface_global_priv *
wpa_supplicant_global_ctrl_iface_init(struct wpa_global *global)
{
	ctrl_iface_global_priv *priv = new ctrl_iface_global_priv;
	priv->global = global;
	StandaloneSupplicant::instance()->wpa_supplicant_global_ctrl_iface_init(priv);
	return priv;
}

void
wpa_supplicant_global_ctrl_iface_deinit(struct ctrl_iface_global_priv *priv)
{
	StandaloneSupplicant::instance()->wpa_supplicant_global_ctrl_iface_deinit(priv);
	delete priv;
}


struct wpa_ctrl * wpa_ctrl_open(const char *ctrl_path)
{
	if (ctrl_path && ctrl_path[0] == '\0') {
		StandaloneSupplicant::instance();
		return NULL;
	}
	StandaloneSupplicant *s = StandaloneSupplicant::instance();
	if (!s->isRunning())
		return NULL;
	struct wpa_ctrl *ctrl = new wpa_ctrl;
	ctrl->ctrl_path = ctrl_path;
	s->wpa_ctrl_open(ctrl);
	return ctrl;
}

void wpa_ctrl_close(struct wpa_ctrl *ctrl)
{
	StandaloneSupplicant::instance()->wpa_ctrl_close(ctrl);
	delete ctrl;
}

int wpa_ctrl_request(struct wpa_ctrl *ctrl, const char *cmd, size_t cmd_len,
		     char *reply, size_t *reply_len,
		     void (*msg_cb)(char *msg, size_t len))
{
	StandaloneSupplicant *s = StandaloneSupplicant::instance();
	if (!s->isRunning())
		return -1;
	return s->wpa_ctrl_request(ctrl, cmd, cmd_len, reply, reply_len);
}

int wpa_ctrl_attach(struct wpa_ctrl *ctrl)
{
	StandaloneSupplicant *s = StandaloneSupplicant::instance();
	if (!s->isRunning())
		return -1;
	return s->StandaloneSupplicant::instance()->wpa_ctrl_attach(ctrl);
}

int wpa_ctrl_detach(struct wpa_ctrl *ctrl)
{
	StandaloneSupplicant *s = StandaloneSupplicant::instance();
	if (!s->isRunning())
		return -1;
	return s->StandaloneSupplicant::instance()->wpa_ctrl_detach(ctrl);
}

int wpa_ctrl_recv(struct wpa_ctrl *ctrl, char *reply, size_t *reply_len)
{
	StandaloneSupplicant *s = StandaloneSupplicant::instance();
	if (!s->isRunning())
		return -1;
	return s->wpa_ctrl_recv(ctrl, reply, reply_len);
}

int wpa_ctrl_pending(struct wpa_ctrl *ctrl)
{
	StandaloneSupplicant *s = StandaloneSupplicant::instance();
	if (!s->isRunning())
		return -1;
	return s->wpa_ctrl_pending(ctrl);
}

int wpa_ctrl_get_fd(struct wpa_ctrl *ctrl)
{
	return -1;
}

int wpa_supplicant_run_call(struct wpa_global *global)
{
	StandaloneSupplicant *s = StandaloneSupplicant::instance();
	return s->wpa_supplicant_run_call(global);
}

}
