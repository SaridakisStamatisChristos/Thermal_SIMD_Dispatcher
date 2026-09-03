#!/bin/bash
set -euo pipefail

echo "--- bootstrap thermal-simd hil runner" > /var/log/thermal-simd-bootstrap.log
apt-get update
apt-get install -y --no-install-recommends awscli build-essential ca-certificates cmake curl git libssl-dev ninja-build python3 linux-tools-common linux-tools-$(uname -r) hwloc numactl

if ! command -v gitlab-runner >/dev/null 2>&1; then
  repo_setup=$(mktemp /tmp/gitlab-runner-repository.XXXXXX.sh)
  curl --fail --show-error --silent --location \
    https://packages.gitlab.com/install/repositories/runner/gitlab-runner/script.deb.sh \
    --output "$repo_setup"
  bash "$repo_setup"
  rm -f -- "$repo_setup"
  apt-get install -y --no-install-recommends gitlab-runner
fi

mkdir -p /etc/systemd/system/gitlab-runner.service.d
cat <<'CONF' >/etc/systemd/system/gitlab-runner.service.d/capabilities.conf
[Service]
AmbientCapabilities=CAP_PERFMON
CapabilityBoundingSet=CAP_PERFMON
SecureBits=keep-caps
CONF

systemctl daemon-reload

runner_authentication_token=""
for _ in $(seq 1 30); do
  if runner_authentication_token=$(aws ssm get-parameter \
    --region "${runner_region}" \
    --name "${runner_authentication_token_parameter_arn}" \
    --with-decryption \
    --query 'Parameter.Value' \
    --output text); then
    break
  fi
  sleep 2
done
case "$runner_authentication_token" in
  glrt-*) ;;
  *)
    echo "SSM parameter does not contain a glrt- runner authentication token" >&2
    exit 1
    ;;
esac

gitlab-runner register --non-interactive \
  --url "${runner_coordinator_url}" \
  --token "$runner_authentication_token" \
  --executor "shell" \
  --description "${runner_name}"
unset runner_authentication_token

systemctl enable --now gitlab-runner
