#!/bin/bash
set -euxo pipefail

echo "--- bootstrap thermal-simd hil runner" > /var/log/thermal-simd-bootstrap.log
apt-get update
apt-get install -y --no-install-recommends git cmake ninja-build python3 python3-pip linux-tools-common linux-tools-$(uname -r) hwloc numactl

useradd --system --create-home --shell /bin/bash gitlab-runner || true

mkdir -p /etc/systemd/system/gitlab-runner.service.d
cat <<'CONF' >/etc/systemd/system/gitlab-runner.service.d/capabilities.conf
[Service]
AmbientCapabilities=CAP_PERFMON CAP_SYS_ADMIN
CapabilityBoundingSet=CAP_PERFMON CAP_SYS_ADMIN
SecureBits=keep-caps
CONF

systemctl daemon-reload

if ! command -v gitlab-runner >/dev/null 2>&1; then
  curl -L https://packages.gitlab.com/install/repositories/runner/gitlab-runner/script.deb.sh | bash
  apt-get install -y gitlab-runner
fi

gitlab-runner register --non-interactive \
  --url "${runner_coordinator_url}" \
  --registration-token "${runner_registration_token}" \
  --executor "shell" \
  --description "${runner_name}" \
  --tag-list "${runner_tags}" \
  --run-untagged=false \
  --locked=true || true

setcap cap_perfmon,cap_sys_admin+ep /usr/bin/perf || true
if [ -f /opt/thermal_simd_dispatcher/bin/thermal_simd ]; then
  setcap cap_perfmon,cap_sys_admin+ep /opt/thermal_simd_dispatcher/bin/thermal_simd || true
fi

systemctl enable --now gitlab-runner
