#!/bin/sh

USER_NAME=builder
SRC_DIR=/src

USER_ID=`stat -c %u $SRC_DIR`
GROUP_ID=`stat -c %g $SRC_DIR`

if ! grep "${USER_ID}:${GROUP_ID}" /etc/passwd >/dev/null 2>&1; then 
    echo "$USER_NAME -> USER ID: $USER_ID, GROUP ID: $GROUP_ID" 
    
    groupadd -g $GROUP_ID $USER_NAME 
    if [ -d "/home/${USER_NAME}" ]; then 
        useradd -M -s /bin/bash -u $USER_ID -g $GROUP_ID -G sudo -c "builder account" $USER_NAME 
    else 
        useradd -m -s /bin/bash -u $USER_ID -g $GROUP_ID -G sudo -c "builder account" $USER_NAME 
    fi 
    
    #cp -rnT /var/config/defaulthome/ /home/$USER_NAME/ # Always return 0. There may be some directories mountes as read-only 
    
    chown -Rf $USER_NAME:$USER_NAME /home/$USER_NAME || : 
    
    export HOME=/home/$USER_NAME 
    export USER=$USER_NAME 
fi

exec /usr/sbin/gosu $USER_NAME /usr/bin/dumb-init "$@"