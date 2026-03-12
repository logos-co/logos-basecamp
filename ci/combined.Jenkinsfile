#!/usr/bin/env groovy

library 'status-jenkins-lib@v1.9.41'

urls = [:]

pipeline {
  agent {
    docker {
      label 'linuxcontainer'
      image 'harbor.status.im/infra/ci-build-containers:linux-base-1.0.1'
      args '--volume=/nix:/nix ' +
           '--volume=/etc/nix:/etc/nix '
    }
  }

  options {
    timestamps()
    disableConcurrentBuilds()
    disableRestartFromStage()
    timeout(time: 60, unit: 'MINUTES')
    buildDiscarder(logRotator(
      numToKeepStr: '10',
      daysToKeepStr: '30',
      artifactNumToKeepStr: '10',
    ))
  }

  parameters {
    booleanParam(
      name: 'PUBLISH',
      description: 'Trigger publishing of build results to GitHub.',
      defaultValue: getPublishDefault(params.PUBLISH),
    )
  }

  stages {
    stage('Build') {
      parallel {
        stage('Linux/x86_64') { steps { script {
          linux_x86_64 = getArtifacts(
            'Linux', jenkins.Build('logos-app/systems/linux/x86_64/package')
          )
        } } }
        stage('macOS/aarch64') { steps { script {
          macos_aarch64 = getArtifacts(
            'macOS', jenkins.Build('logos-app/systems/macos/aarch64/package')
          )
        } } }
      }
    }

    stage('Publish') {
      when { expression { params.PUBLISH } }
      steps { script {
        github.publishReleaseFiles(user: 'logos-co', repo: 'logos-app')
      } }
    }
  }

  post {
    always { script {
      dir('pkg') {
        sha = "./${utils.pkgFilename(name: 'LogosApp', ext: 'sha256')}"
        sh "sha256sum * | tee ${sha}"
        urls['SHA'] = s5cmd.upload(sha)
        jenkins.setBuildDesc(urls)
      }
      archiveArtifacts('pkg/*')
    } }
    cleanup {
      cleanWs(disableDeferredWipeout: true)
      dir(env.WORKSPACE_TMP) { deleteDir() }
    }
  }
}

/* Helper that makes PUBLISH default to 'false' unless:
 * - The build is for a release branch
 * - A user explicitly specified a value
 * Since release builds create and re-create GitHub drafts every time. */
def Boolean getPublishDefault(Boolean previousValue) {
  if (env.JOB_NAME.startsWith('logos-app/release')) { return true }
  if (previousValue != null) { return previousValue }
  return false
}

/* Helper for getting artifacts from child builds. */
def getArtifacts(key, childBuild) {
  /* Copy artifacts from child build to parent. */
  jenkins.copyArts(childBuild)
  /* Archive right away to make artifacts available. */
  archiveArtifacts('pkg/*')
  /* Add new URL from child build and update description. */
  urls[key] = utils.pkgUrl(childBuild)
  jenkins.setBuildDesc(urls)
  return childBuild
}