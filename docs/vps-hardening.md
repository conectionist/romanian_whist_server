# Hardening the VPS

The README's [On a VPS, step by step](../README.md#on-a-vps-step-by-step) gets the game running.
These steps make the machine itself safer, and none of them is needed for the game to work. Do them
from a second SSH session, keeping the first one open until the new settings are tested, and know
where your provider's web console is: it gets you in when SSH does not.

## SSH: keys only

A fresh VPS often allows password logins, root's included, and bots try passwords on every public
address. Check:

```
sudo sshd -T | grep -E 'passwordauthentication|permitrootlogin'
```

If either says `yes`, first log in with a key, then turn passwords off.

1. On your own computer, make a key if you have none (`ls ~/.ssh/id_ed25519.pub`), with a passphrase:
   ```
   ssh-keygen -t ed25519
   ```
2. Copy it to the VPS, which asks for the password one last time, then check that a new login takes
   the key and asks for no server password:
   ```
   ssh-copy-id root@<the VPS's address>
   ssh root@<the VPS's address>
   ```
3. On the VPS, turn passwords off. A file whose name sorts first wins over the others in that folder,
   such as a cloud-init one that turns passwords back on:
   ```
   printf 'PasswordAuthentication no\nKbdInteractiveAuthentication no\nPermitRootLogin prohibit-password\n' | sudo tee /etc/ssh/sshd_config.d/00-hardening.conf
   sudo sshd -t && sudo systemctl reload ssh
   ```
4. From your computer, with the old session still open: `ssh root@<the VPS's address>` must work, and
   `ssh -o PubkeyAuthentication=no root@<the VPS's address>` must fail with
   `Permission denied (publickey)`. If the first fails, undo it from the old session:
   ```
   sudo rm /etc/ssh/sshd_config.d/00-hardening.conf && sudo systemctl reload ssh
   ```

Keep the private key backed up: without it, only the web console gets you in.

## A firewall

Allow in only SSH (22/tcp, or your own port), 80/tcp, 443/tcp and 443/udp, and drop the rest. Either
of these does:

- **The provider's firewall**, in its web interface, where there is one: an `in`, `ACCEPT` rule for
  each of the four, then the input policy set to `DROP`. It filters before the machine, so it covers
  the ports Docker publishes too.
- **`ufw`** on the machine (`sudo apt-get install -y ufw`), as in the README's step 2. Docker
  publishes its ports whatever `ufw` says, so it guards only what else listens.

Add the SSH rule before the policy that drops the rest, or the next login is refused.

## Later, if wanted

- **Updates:** `unattended-upgrades` installs the security updates by itself
  (`sudo apt-get install -y unattended-upgrades`, then `sudo dpkg-reconfigure -plow unattended-upgrades`).
- **A user of your own** with `sudo`, and `PermitRootLogin no`, so that root cannot log in at all.
- **`fail2ban`**, which blocks addresses that keep failing to log in. With passwords off it matters
  less, but it keeps the logs quieter.
- **Tools left logged in:** a coding agent or other tool signed in to an account on the VPS keeps its
  credentials there. Log it out when the work is done.
