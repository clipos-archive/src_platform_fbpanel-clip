#!/bin/sh
# SPDX-License-Identifier: LGPL-2.1-or-later
# Copyright © 2007-2018 ANSSI. All Rights Reserved.
# Reboot helper for CLIP livecd
# Copyright (c) 2010 ANSSI
# Author: Vincent Strubel <clipos@ssi.gouv.fr>
# Distributed under the terms of the GNU General Public Licence,
# version 2.

case "${1}" in 
	reboot)
		TITLE="Redémarrage"
		MSG="Confirmez-vous le redémarrage du système ?"
		ACTION="/sbin/reboot"
		;;
	halt)
		TITLE="Arrêt"
		MSG="Confirmez-vous l'arrêt du système ?"
		ACTION="/sbin/halt"
		;;
	*)
		echo "Unsupported argument: ${1}" >&2
		exit 1
		;;
esac

Xdialog --title "${TITLE}" --yesno "${MSG}" 0 0 
[[ $? -eq 0 ]] || exit 0
exec ${ACTION}
