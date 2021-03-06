#!/bin/sh
# Copyright (C) 2011-2012 Red Hat, Inc. All rights reserved.
#
# This copyrighted material is made available to anyone wishing to use,
# modify, copy, or redistribute it subject to the terms and conditions
# of the GNU General Public License v.2.
#
# You should have received a copy of the GNU General Public License
# along with this program; if not, write to the Free Software Foundation,
# Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA

. lib/test

# is_in_sync <VG/LV>
is_in_sync_() {
	local a
	local b
	local idx
	local dm_name=$(echo $1 | sed s:-:--: | sed s:/:-:)

	if ! a=(`dmsetup status $dm_name`); then
		echo "Unable to get sync status of $1"
		return 1
	elif [ ${a[2]} = "snapshot-origin" ]; then
		if ! a=(`dmsetup status ${dm_name}-real`); then
			echo "Unable to get sync status of $1"
			return 1
		fi
	fi

	# 6th argument is the sync ratio for RAID and mirror
	if [ ${a[2]} = "raid" ]; then
		# Last argument is the sync ratio for RAID
		idx=$((${#a[@]} - 1))
	elif [ ${a[2]} = "mirror" ]; then
		# 4th Arg tells us how far to the sync ratio
		idx=$((${a[3]} + 4))
	else
		echo "Unable to get sync ratio for target type '${a[2]}'"
		return 1
	fi
	b=( $(echo ${a[$idx]} | sed s:/:' ':) )

	if [ ${b[0]} != ${b[1]} ]; then
		echo "$dm_name (${a[3]}) is not in-sync"
		return 1
	fi

	if [[ ${a[$(($idx - 1))]} =~ a ]]; then
		echo "$dm_name in-sync, but 'a' characters in health status"
		return 1
	fi

	if [ ${a[2]} = "raid" ]; then
		echo "$dm_name (${a[3]}) is in-sync"
	else
		echo "$dm_name (${a[2]}) is in-sync"
	fi
}

# wait_for_sync <VG/LV>
wait_for_sync_() {
	for i in {1..100} ; do
		is_in_sync_ $1 && return
		sleep 1
	done

	echo "Sync is taking too long - assume stuck"
	return 1
}

########################################################
# MAIN
########################################################
aux target_at_least dm-raid 1 1 0 || skip

aux prepare_pvs 5 80
vgcreate -c n -s 256k $vg $(cat DEVICES)

###########################################
# RAID1 convert tests
###########################################
#
# FIXME: Snapshots of RAID is available, but there are kernel bugs that
#        still prevent its use.
#for under_snap in false true; do
for under_snap in false; do
for i in 1 2 3 4; do
	for j in 1 2 3 4; do
		if [ $i -eq 1 ]; then
			from="linear"
		else
			from="$i-way"
		fi
		if [ $j -eq 1 ]; then
			to="linear"
		else
			to="$j-way"
		fi

		echo -n "Converting from $from to $to"
		if $under_snap; then
			echo -n " (while under a snapshot)"
		fi
		echo

		if [ $i -eq 1 ]; then
			# Shouldn't be able to create with just 1 image
			not lvcreate --type raid1 -m 0 -l 2 -n $lv1 $vg

			lvcreate -l 2 -n $lv1 $vg
		else
			lvcreate --type raid1 -m $(($i - 1)) -l 2 -n $lv1 $vg
			wait_for_sync_ $vg/$lv1
		fi

		if $under_snap; then
			lvcreate -s $vg/$lv1 -n snap -l 2
		fi

		lvconvert -m $((j - 1))  $vg/$lv1

		# FIXME: ensure no residual devices

		if [ $j -eq 1 ]; then
			check linear $vg $lv1
		fi
		lvremove -ff $vg
	done
done
done

# 3-way to 2-way convert while specifying devices
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg $dev1 $dev2 $dev3
wait_for_sync_ $vg/$lv1
lvconvert -m1 $vg/$lv1 $dev2
lvremove -ff $vg

#
# FIXME: Add tests that specify particular devices to be removed
#

###########################################
# RAID1 split tests
###########################################
# 3-way to 2-way/linear
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
wait_for_sync_ $vg/$lv1
lvconvert --splitmirrors 1 -n $lv2 $vg/$lv1
check lv_exists $vg $lv1
check linear $vg $lv2
# FIXME: ensure no residual devices
lvremove -ff $vg

# 2-way to linear/linear
lvcreate --type raid1 -m 1 -l 2 -n $lv1 $vg
wait_for_sync_ $vg/$lv1
lvconvert --splitmirrors 1 -n $lv2 $vg/$lv1
check linear $vg $lv1
check linear $vg $lv2
# FIXME: ensure no residual devices
lvremove -ff $vg

# 3-way to linear/2-way
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
wait_for_sync_ $vg/$lv1
# FIXME: Can't split off a RAID1 from a RAID1 yet
should lvconvert --splitmirrors 2 -n $lv2 $vg/$lv1
#check linear $vg $lv1
#check lv_exists $vg $lv2
# FIXME: ensure no residual devices
lvremove -ff $vg

###########################################
# RAID1 split + trackchanges / merge
###########################################
# 3-way to 2-way/linear
lvcreate --type raid1 -m 2 -l 2 -n $lv1 $vg
wait_for_sync_ $vg/$lv1
lvconvert --splitmirrors 1 --trackchanges $vg/$lv1
check lv_exists $vg $lv1
check linear $vg ${lv1}_rimage_2
lvconvert --merge $vg/${lv1}_rimage_2
# FIXME: ensure no residual devices
lvremove -ff $vg

###########################################
# Mirror to RAID1 conversion
###########################################
for i in 1 2 3 ; do
	lvcreate --type mirror -m $i -l 2 -n $lv1 $vg
	wait_for_sync_ $vg/$lv1
	lvconvert --type raid1 $vg/$lv1
	lvremove -ff $vg
done
