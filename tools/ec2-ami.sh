#!/bin/sh
# Turns build/bastios-usb.img into an EC2 AMI (docs/cloud.md, "Booting on EC2").
#
#   AWS_REGION=eu-west-1 BUCKET=my-scratch-bucket tools/ec2-ami.sh [IMAGE]
#
# Needs the AWS CLI with credentials that may use S3 and EC2, and the
# `vmimport` service role (see "Importing a disk image" in the AWS VM
# Import/Export guide). Prints the AMI id. The image boots under legacy BIOS
# on any x86-64 Nitro instance type with an ENA card (t3, m5, c5 ...).
set -eu

img=${1:-build/bastios-usb.img}
: "${BUCKET:?set BUCKET to an S3 bucket in the region}"
: "${AWS_REGION:?set AWS_REGION}"
export AWS_DEFAULT_REGION=$AWS_REGION
key=bastios/$(basename "$img").$$

aws s3 cp "$img" "s3://$BUCKET/$key"
task=$(aws ec2 import-snapshot --description BastiOS \
  --disk-container "Format=RAW,UserBucket={S3Bucket=$BUCKET,S3Key=$key}" \
  --query ImportTaskId --output text)
echo "import task $task" >&2

while :; do
  st=$(aws ec2 describe-import-snapshot-tasks --import-task-ids "$task" \
    --query 'ImportSnapshotTasks[0].SnapshotTaskDetail.[Status,StatusMessage]' --output text)
  case $st in
    completed*) break ;;
    deleted*|deleting*) echo "import failed: $st" >&2; exit 1 ;;
  esac
  sleep 10
done
snap=$(aws ec2 describe-import-snapshot-tasks --import-task-ids "$task" \
  --query 'ImportSnapshotTasks[0].SnapshotTaskDetail.SnapshotId' --output text)
aws s3 rm "s3://$BUCKET/$key" >/dev/null

aws ec2 register-image --name "bastios-$(date +%Y%m%d-%H%M%S)" \
  --architecture x86_64 --virtualization-type hvm --boot-mode legacy-bios \
  --ena-support --root-device-name /dev/xvda \
  --block-device-mappings "DeviceName=/dev/xvda,Ebs={SnapshotId=$snap,VolumeType=gp3}" \
  --query ImageId --output text
