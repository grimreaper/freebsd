/*
 * wpa_gui - ScanResults class
 * Copyright (c) 2005-2006, Jouni Malinen <j@w1.fi>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * Alternatively, this software may be distributed under the terms of BSD
 * license.
 *
 * See README and COPYING for more details.
 */

#include <QTimer>

#include <cstdio>

#include "scanresults.h"
#include "wpagui.h"
#include "networkconfig.h"


ScanResults::ScanResults(QWidget *parent, const char *, bool, Qt::WFlags)
	: QDialog(parent)
{
	setupUi(this);

	connect(closeButton, SIGNAL(clicked()), this, SLOT(close()));
	connect(scanButton, SIGNAL(clicked()), this, SLOT(scanRequest()));
	connect(scanResultsView, SIGNAL(doubleClicked(Q3ListViewItem *)), this,
		SLOT(bssSelected(Q3ListViewItem *)));

	wpagui = NULL;
}


ScanResults::~ScanResults()
{
	delete timer;
}


void ScanResults::languageChange()
{
	retranslateUi(this);
}


void ScanResults::setWpaGui(WpaGui *_wpagui)
{
	wpagui = _wpagui;
	updateResults();
    
	timer = new QTimer(this);
	connect(timer, SIGNAL(timeout()), SLOT(getResults()));
	timer->start(10000, FALSE);
}


void ScanResults::updateResults()
{
	char reply[8192];
	size_t reply_len;
    
	if (wpagui == NULL)
		return;

	reply_len = sizeof(reply) - 1;
	if (wpagui->ctrlRequest("SCAN_RESULTS", reply, &reply_len) < 0)
		return;
	reply[reply_len] = '\0';

	scanResultsView->clear();
    
	QString res(reply);
	QStringList lines = QStringList::split(QChar('\n'), res);
	bool first = true;
	for (QStringList::Iterator it = lines.begin(); it != lines.end(); it++)
	{
		if (first) {
			first = false;
			continue;
		}
	
		QStringList cols = QStringList::split(QChar('\t'), *it, true);
		QString ssid, bssid, freq, signal, flags;
		bssid = cols.count() > 0 ? cols[0] : "";
		freq = cols.count() > 1 ? cols[1] : "";
		signal = cols.count() > 2 ? cols[2] : "";
		flags = cols.count() > 3 ? cols[3] : "";
		ssid = cols.count() > 4 ? cols[4] : "";
		new Q3ListViewItem(scanResultsView, ssid, bssid, freq, signal,
				   flags);
	}
}


void ScanResults::scanRequest()
{
	char reply[10];
	size_t reply_len = sizeof(reply);
    
	if (wpagui == NULL)
		return;
    
	wpagui->ctrlRequest("SCAN", reply, &reply_len);
}


void ScanResults::getResults()
{
	updateResults();
}


void ScanResults::bssSelected( Q3ListViewItem * sel )
{
	NetworkConfig *nc = new NetworkConfig();
	if (nc == NULL)
		return;
	nc->setWpaGui(wpagui);
	nc->paramsFromScanResults(sel);
	nc->show();
	nc->exec();
}
